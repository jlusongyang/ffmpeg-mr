#include <jni.h>
#include "com_tstordyallison_ffmpegmr_Demuxer.h"
#include "com_tstordyallison_ffmpegmr_Demuxer_DemuxPacket.h"

#include <map>
using namespace std;

extern "C" {
    #include "ffmpeg_tpl.h"

    #include "libavcodec/avcodec.h"
    #include "libavutil/mathematics.h"
    #include "libavutil/imgutils.h"
    #include "libavutil/dict.h"
    #include "libavformat/avformat.h"
    #include "libavutil/mathematics.h"
}

// Util methods.
// --------

static void print_file_error(const char *filename, int err)
{
    char errbuf[128];
    const char *errbuf_ptr = errbuf;
    
    if (av_strerror(err, errbuf, sizeof(errbuf)) < 0)
        errbuf_ptr = strerror(AVUNERROR(err));
    fprintf(stderr, "%s: %s\n", filename, errbuf_ptr);
}

static int getHashCode(JNIEnv* env, jobject obj)
{
    jclass clazz  = env->GetObjectClass(obj);
    jmethodID mid = env->GetMethodID(clazz, "hashCode", "()I");
    jint hashCode = env->CallIntMethod(obj, mid);
    return (int)hashCode;
}


// Main Demuxer State List.
// --------

struct DemuxState {
    AVFormatContext *fmt_ctx;
    AVPacket pkt;
    int     stream_count;
    uint8_t **stream_data;
    int     *stream_data_sizes;
    jclass  dpkt_clazz;
    jmethodID dpkt_ctr;    
};
const struct DemuxState DEMUXSTATE_DEFAULT = {NULL, NULL, -1, NULL, NULL, NULL, NULL};

static map<int, DemuxState*> objectRegister; // Object register.

static DemuxState *getObjectState(JNIEnv *env, jobject obj)
{
    int hashCode = getHashCode(env, obj);
    if(objectRegister.find(hashCode) != objectRegister.end())
    {
        return objectRegister[hashCode];
    }
    return NULL;
}

static void unregisterObjectState(JNIEnv *env, jobject obj)
{
    DemuxState *state = getObjectState(env, obj);
    if(state != NULL)
    {
        
        // Free up the streams data.
        if(state->stream_data != NULL)
        {
            for(int i = 0; i < state->stream_count; i++)
            {
                if(state->stream_data[i] != NULL)
                    free(state->stream_data[i]); // free as these came from tpl.
            }
        }
        delete[] state->stream_data;
        
        // Free the stream size data.
        if(state->stream_data_sizes != NULL)
            delete[] state->stream_data_sizes;
        
        // Close the format context (TODO: think about what to do here if we are stream).
        if(state->fmt_ctx != NULL)
            av_close_input_file(state->fmt_ctx);
        
        // Remove the state from the map.
        int hashCode = getHashCode(env, obj);
        objectRegister.erase(hashCode);
        
        // Free the state.
        delete state;
    }
}

static void registerObjectState(JNIEnv *env, jobject obj, DemuxState *objstate)
{
    int hashCode = getHashCode(env, obj);
    objectRegister[hashCode] = objstate;
}

// JNI Methods.
// --------

JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM *vm, void *reserved){
    // Load up FFmpeg.
    av_register_all();
    avcodec_register_all();
    
    // Return the JVM version we need to run.
    return (jint)JNI_VERSION_1_6;
}


/*
 * Class:     com_tstordyallison_ffmpegmr_Demuxer
 * Method:    initDemuxWithFile
 * Signature: (Ljava/lang/String;)V
 */
JNIEXPORT jint JNICALL Java_com_tstordyallison_ffmpegmr_Demuxer_initDemuxWithFile
(JNIEnv *env, jobject obj, jstring jfilename){

    // Local vars.
    int err = 0;
    int i, filename_copy;
    const char *filename;
    
    // Init state and add them to the object register.
    DemuxState *state = new DemuxState;
    *state = DEMUXSTATE_DEFAULT;
    registerObjectState(env, obj, state);
    
    // Opens the file, reads the streams, generates stream TPL, prepares for read calls.
    // ---------
    
    // Open the file for reading.
    filename = env->GetStringUTFChars(jfilename, (jboolean *)&filename_copy);
    if ((err = avformat_open_input(&state->fmt_ctx, filename, NULL, NULL)) < 0) {
        print_file_error(filename, err);
        goto failure;
    }
    
    // Fill the streams in the format context
    if ((err = avformat_find_stream_info(state->fmt_ctx, NULL)) < 0) {
        printf("Failed to open streams in file %s, error %d\n", filename, err);
        goto failure;
    }
    
    // Bind a decoder to each input stream, and generate the TPL stream headers.
    state->stream_count      = state->fmt_ctx->nb_streams;
    state->stream_data       = new uint8_t*[state->stream_count]; // list of lists
    state->stream_data_sizes = new int[state->stream_count];
    
    for (i = 0; i < state->fmt_ctx->nb_streams; i++) {
        AVStream *stream = state->fmt_ctx->streams[i];
        AVCodec *codec;
        
        if (!(codec = avcodec_find_decoder(stream->codec->codec_id))) {
            //fprintf(stderr, "Warning: Unsupported codec with id %d for input stream %d\n", stream->codec->codec_id, stream->index);
            state->stream_data[stream->index] = NULL;
            state->stream_data_sizes[stream->index] = 0;
            continue;
        } else if (avcodec_open2(stream->codec, codec, NULL) < 0) {
            fprintf(stderr, "Error while opening codec for input stream %d\n", stream->index);
            err = -1;
            goto failure;
        }
        
        // Generate the TPL stream headers.
        err = write_avstream_chunk_to_memory(stream, &((state->stream_data)[i]), &((state->stream_data_sizes)[i])); // I will be amazed if this works.
    }
    
    // Init the packet storage.
    av_init_packet(&state->pkt);
    
    // Do some once only init on the dpkt object.
    state->dpkt_clazz = env->FindClass("com/tstordyallison/ffmpegmr/Demuxer$DemuxPacket");
    if(state->dpkt_clazz == NULL) 
    {
        fprintf(stderr, "Could not find the com/tstordyallison/ffmpegmr/Demuxer$DemuxPacket class in the JVM.\n");
        err = -1;
        goto failure;
    }
    state->dpkt_ctr = env->GetMethodID(state->dpkt_clazz, "<init>", "()V");
    
failure:
    // Deallocs
    if(filename_copy == JNI_TRUE)
        env->ReleaseStringUTFChars(jfilename, filename);
    
    if(err != 0){
        unregisterObjectState(env, obj);
        return err;
    }else
        return 0;

}

typedef struct StreamRead {
    int stream_idx;
    DemuxState *objstate;
} StreamRead;

/*
 * Custom read function for TPL stream data.
 */
static int StreamData_Read(/*StreamRead*/ void *stream_choice, uint8_t *buf, int buf_size){
    return -1;
}

/*
 * Class:     com_tstordyallison_ffmpegmr_Demuxer
 * Method:    initDemuxWithStream
 * Signature: (Ljava/io/InputStream;)V
 */
JNIEXPORT jint JNICALL Java_com_tstordyallison_ffmpegmr_Demuxer_initDemuxWithStream
(JNIEnv *env, jobject obj, jobject stream){
    // Not yet implemented - "Use av_open_input_stream() instead of av_open_input_file() to open your stream".
    // Define a bunch of functions that can then read the stream.
    // Going to be a bit messy.
    return -1;
}


/*
 * FFmpeg custom IO read function - for use with JNI InputStream.
 */
static int Java_InputStream_Read(/*jobject*/ void *jni_input_stream, uint8_t *buf, int buf_size){
    return -1;
}

/*
 * FFmpeg custom IO seek function - for use with JNI InputStream.
 */

static int64_t Java_InputStream_Seek(/*jobject*/ void *jni_input_stream, int64_t offset, int whence){
    return -1;
}


/*
 * Class:     com_tstordyallison_ffmpegmr_Demuxer
 * Method:    getStreamData
 * Signature: (I)[B
 */
JNIEXPORT jbyteArray JNICALL Java_com_tstordyallison_ffmpegmr_Demuxer_getStreamData
(JNIEnv * env, jobject obj, jint stream_idx){
    DemuxState *state = getObjectState(env, obj);
    if(state != NULL)
    {
        jbyteArray output = env->NewByteArray(state->stream_data_sizes[stream_idx]);
        env->SetByteArrayRegion(output, 0, state->stream_data_sizes[stream_idx], (jbyte*)(state->stream_data[stream_idx]));
        return output;
    }
    else
        return NULL;
}



/*
 * Class:     com_tstordyallison_ffmpegmr_Demuxer
 * Method:    getStreamCount
 * Signature: ()I
 */
JNIEXPORT jint JNICALL Java_com_tstordyallison_ffmpegmr_Demuxer_getStreamCount
(JNIEnv * env, jobject obj){
    DemuxState *state = getObjectState(env, obj);
    if(state != NULL)
    {
        return state->stream_count;
    }
    else
        return -1;
}


/*
 * Calculates the timestamp value that we use when we are synchronising the streams. 
 *
 * **** This is a hack for now - we just represent everything in MICROSECONDS (1/1,000,000 of a second). ****
 * **** We really need to pass over the timebase, and do the comparisons properly. But if we can get away with this... ****
 *
 * Used mainly for chunking the data up accurately, and merging it back together in the reduce.
 */
static long getPacketTimeStamp(AVStream *stream, AVPacket pkt){
    AVRational ts_base = {1, 1000000};
    return av_rescale_q(pkt.dts, stream->time_base, ts_base);
}

/*
 * Calculates the duration of the packet based in microseconds.
 *
 * Used mainly for chunking the data up accurately, and merging it back together in the reduce.
 */
static long getPacketDuration(AVStream *stream, AVPacket pkt){
    AVRational ts_base = {1, 1000000};
    return av_rescale_q(pkt.duration, stream->time_base, ts_base);
}



/*
 * Class:     com_tstordyallison_ffmpegmr_Demuxer
 * Method:    getNextChunk
 * Signature: ()Lcom/tstordyallison/ffmpegmr/Demuxer/DemuxPacket;
 *
 * Hopefully this method won't slow the whole thing down too much. If it does, we are screwed.
 */
JNIEXPORT jobject JNICALL Java_com_tstordyallison_ffmpegmr_Demuxer_getNextChunkImpl
(JNIEnv * env, jobject obj){
    DemuxState *state = getObjectState(env, obj);
    if(state != NULL)
    {
        // Read the next AVPacket from the input.
        int ret = 0;
        ret = av_read_frame(state->fmt_ctx, &state->pkt);
        if(ret != 0) return NULL;
        
        // Skip over any invalid streams.
        while(state->stream_data_sizes[state->pkt.stream_index] == 0){
            ret = av_read_frame(state->fmt_ctx, &state->pkt);
            if(ret != 0) return NULL;
        }
        
        // Temp for the AVPacket TPL.
        uint8_t *pkt_tpl_data;
        int pkt_tpl_size;                               
        
        // Generate the TPL.
        write_avpacket_chunk_to_memory(&state->pkt, &pkt_tpl_data, &pkt_tpl_size);
        av_free_packet(&state->pkt);
                                       
        // Create the DemuxPacket object.
        jclass      dpkt_clazz = env->FindClass("com/tstordyallison/ffmpegmr/Demuxer$DemuxPacket");
        jmethodID   dpkt_ctr = env->GetMethodID(dpkt_clazz, "<init>", "()V");
        jobject     dpkt = env->NewObject(dpkt_clazz, dpkt_ctr);
        
        // Copy the TPL version to the dpkt (and fill in the other dpkt values).
        jfieldID streamID = env->GetFieldID(dpkt_clazz, "streamID", "I");
        jfieldID ts = env->GetFieldID(dpkt_clazz, "ts", "J");
        jfieldID duration = env->GetFieldID(dpkt_clazz, "duration", "J");
        jfieldID splitPoint = env->GetFieldID(dpkt_clazz, "splitPoint", "Z");
        jfieldID data = env->GetFieldID(dpkt_clazz, "data", "Ljava/nio/ByteBuffer;");
        
        env->SetIntField(dpkt, streamID, state->pkt.stream_index);
        env->SetLongField(dpkt, duration, getPacketDuration(state->fmt_ctx->streams[state->pkt.stream_index], state->pkt));
        env->SetLongField(dpkt, ts, getPacketTimeStamp(state->fmt_ctx->streams[state->pkt.stream_index], state->pkt));
        
        if(state->pkt.flags & AV_PKT_FLAG_KEY)
            env->SetBooleanField(dpkt, splitPoint, JNI_TRUE);
        else
            env->SetBooleanField(dpkt, splitPoint, JNI_FALSE);
        
        jobject buffer = env->NewDirectByteBuffer(pkt_tpl_data, (jlong)pkt_tpl_size);
        env->SetObjectField(dpkt, data, buffer);
        
        // The buffer will be released when DemuxPacket_deallocData is called below.
        
        // Done.
        return dpkt;
    }
    else{
        fprintf(stderr, "Warning: failed to find object for a getNextChunk() call.\n");
        return NULL;
    }
}


/*
 * Class:     com_tstordyallison_ffmpegmr_Demuxer_DemuxPacket
 * Method:    deallocData
 * Signature: ()V
 */
JNIEXPORT jint JNICALL Java_com_tstordyallison_ffmpegmr_Demuxer_00024DemuxPacket_deallocData
(JNIEnv * env, jobject obj)
{
    
    jclass      dpkt_clazz = env->FindClass("com/tstordyallison/ffmpegmr/Demuxer$DemuxPacket");
    jfieldID    data =       env->GetFieldID(dpkt_clazz, "data", "Ljava/nio/ByteBuffer;");
    jobject     buffer =     env->GetObjectField(obj, data);
    if(buffer != NULL)
    {
        void *pkt_tpl_data = env->GetDirectBufferAddress(buffer);
        free(pkt_tpl_data);
        env->SetObjectField(obj, data, NULL); 
        return 0;
    }
    else
    {
        return -1;
    }
}

/*
 * Class:     com_tstordyallison_ffmpegmr_Demuxer
 * Method:    close
 * Signature: ()I
 */
JNIEXPORT jint JNICALL Java_com_tstordyallison_ffmpegmr_Demuxer_close
(JNIEnv * env, jobject obj){
    DemuxState *state = getObjectState(env, obj);
    if(state != NULL)
    {
        // Unregister the object state.
        unregisterObjectState(env, obj);

        return 0;
    }
    else
        return -1;
}