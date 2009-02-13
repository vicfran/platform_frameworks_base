/*
 * Copyright (C) 2008 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#define LOG_NDEBUG 0

#define LOG_TAG "AudioTrack-JNI"

#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <math.h>

#include "jni.h"
#include "JNIHelp.h"
#include "android_runtime/AndroidRuntime.h"

#include "utils/Log.h"
#include "media/AudioSystem.h"
#include "media/AudioTrack.h"

#include <utils/MemoryHeapBase.h>
#include <utils/MemoryBase.h>


// ----------------------------------------------------------------------------

using namespace android;

// ----------------------------------------------------------------------------
static const char* const kClassPathName = "android/media/AudioTrack";

struct fields_t {
    // these fields provide access from C++ to the...
    jclass    audioTrackClass;       //... AudioTrack class
    jmethodID postNativeEventInJava; //... event post callback method
    int       PCM16;                 //...  format constants
    int       PCM8;                  //...  format constants
    int       STREAM_VOICE_CALL;     //...  stream type constants
    int       STREAM_SYSTEM;         //...  stream type constants
    int       STREAM_RING;           //...  stream type constants
    int       STREAM_MUSIC;          //...  stream type constants
    int       STREAM_ALARM;          //...  stream type constants
    int       STREAM_NOTIFICATION;   //...  stream type constants
    int       STREAM_BLUETOOTH_SCO;   //...  stream type constants
    int       MODE_STREAM;           //...  memory mode
    int       MODE_STATIC;           //...  memory mode
    jfieldID  nativeTrackInJavaObj; // stores in Java the native AudioTrack object
    jfieldID  jniData;      // stores in Java additional resources used by the native AudioTrack
};
static fields_t javaAudioTrackFields;

struct audiotrack_callback_cookie {
    jclass      audioTrack_class;
    jobject     audioTrack_ref;
 };

// ----------------------------------------------------------------------------
class AudioTrackJniStorage {
    public:
        sp<MemoryHeapBase>         mMemHeap;
        sp<MemoryBase>             mMemBase;
        audiotrack_callback_cookie mCallbackData;
        int                        mStreamType;

    AudioTrackJniStorage() {
    }

    ~AudioTrackJniStorage() {
        mMemBase.clear();
        mMemHeap.clear();
    }

    bool allocSharedMem(int sizeInBytes) {
        mMemHeap = new MemoryHeapBase(sizeInBytes, 0, "AudioTrack Heap Base");
        if (mMemHeap->getHeapID() < 0) {
            return false;
        }
        mMemBase = new MemoryBase(mMemHeap, 0, sizeInBytes);
        return true;
    }
};

// ----------------------------------------------------------------------------
#define DEFAULT_OUTPUT_SAMPLE_RATE   44100

#define AUDIOTRACK_SUCCESS                         0
#define AUDIOTRACK_ERROR                           -1
#define AUDIOTRACK_ERROR_BAD_VALUE                 -2
#define AUDIOTRACK_ERROR_INVALID_OPERATION         -3
#define AUDIOTRACK_ERROR_SETUP_AUDIOSYSTEM         -16
#define AUDIOTRACK_ERROR_SETUP_INVALIDCHANNELCOUNT -17
#define AUDIOTRACK_ERROR_SETUP_INVALIDFORMAT       -18
#define AUDIOTRACK_ERROR_SETUP_INVALIDSTREAMTYPE   -19
#define AUDIOTRACK_ERROR_SETUP_NATIVEINITFAILED    -20


jint android_media_translateErrorCode(int code) {
    switch(code) {
    case NO_ERROR:
        return AUDIOTRACK_SUCCESS;
    case BAD_VALUE:
        return AUDIOTRACK_ERROR_BAD_VALUE;
    case INVALID_OPERATION:
        return AUDIOTRACK_ERROR_INVALID_OPERATION;
    default:
        return AUDIOTRACK_ERROR;
    }   
}


// ----------------------------------------------------------------------------
static void audioCallback(int event, void* user, void *info) {
    if (event == AudioTrack::EVENT_MORE_DATA) {
        // set size to 0 to signal we're not using the callback to write more data
        AudioTrack::Buffer* pBuff = (AudioTrack::Buffer*)info;
        pBuff->size = 0;  
    
    } else if (event == AudioTrack::EVENT_MARKER) {
        audiotrack_callback_cookie *callbackInfo = (audiotrack_callback_cookie *)user;
        JNIEnv *env = AndroidRuntime::getJNIEnv();
        if (user && env) {
            env->CallStaticVoidMethod(
                callbackInfo->audioTrack_class, 
                javaAudioTrackFields.postNativeEventInJava,
                callbackInfo->audioTrack_ref, event, 0,0, NULL);
            if (env->ExceptionCheck()) {
                env->ExceptionDescribe();
                env->ExceptionClear();
            }
        }

    } else if (event == AudioTrack::EVENT_NEW_POS) {
        audiotrack_callback_cookie *callbackInfo = (audiotrack_callback_cookie *)user;
        JNIEnv *env = AndroidRuntime::getJNIEnv();
        if (user && env) {
            env->CallStaticVoidMethod(
                callbackInfo->audioTrack_class, 
                javaAudioTrackFields.postNativeEventInJava,
                callbackInfo->audioTrack_ref, event, 0,0, NULL);
            if (env->ExceptionCheck()) {
                env->ExceptionDescribe();
                env->ExceptionClear();
            }
        }
    }
}


// ----------------------------------------------------------------------------
static int
android_media_AudioTrack_native_setup(JNIEnv *env, jobject thiz, jobject weak_this,
        jint streamType, jint sampleRateInHertz, jint nbChannels, 
        jint audioFormat, jint buffSizeInBytes, jint memoryMode)
{
    //LOGV("sampleRate=%d, audioFormat(from Java)=%d, nbChannels=%d, buffSize=%d", 
    //    sampleRateInHertz, audioFormat, nbChannels, buffSizeInBytes);
    int afSampleRate;
    int afFrameCount;

    if (AudioSystem::getOutputFrameCount(&afFrameCount, streamType) != NO_ERROR) {
        LOGE("Error creating AudioTrack: Could not get AudioSystem frame count.");
        return AUDIOTRACK_ERROR_SETUP_AUDIOSYSTEM;
    }
    if (AudioSystem::getOutputSamplingRate(&afSampleRate, streamType) != NO_ERROR) {
        LOGE("Error creating AudioTrack: Could not get AudioSystem sampling rate.");
        return AUDIOTRACK_ERROR_SETUP_AUDIOSYSTEM;
    }

    if ((nbChannels == 0) || (nbChannels > 2)) {
        LOGE("Error creating AudioTrack: channel count is not 1 or 2.");
        return AUDIOTRACK_ERROR_SETUP_INVALIDCHANNELCOUNT;
    }
    
    // check the stream type
    AudioSystem::stream_type atStreamType;
    if (streamType == javaAudioTrackFields.STREAM_VOICE_CALL) {
        atStreamType = AudioSystem::VOICE_CALL;
    } else if (streamType == javaAudioTrackFields.STREAM_SYSTEM) {
        atStreamType = AudioSystem::SYSTEM;
    } else if (streamType == javaAudioTrackFields.STREAM_RING) {
        atStreamType = AudioSystem::RING;
    } else if (streamType == javaAudioTrackFields.STREAM_MUSIC) {
        atStreamType = AudioSystem::MUSIC;
    } else if (streamType == javaAudioTrackFields.STREAM_ALARM) {
        atStreamType = AudioSystem::ALARM;
    } else if (streamType == javaAudioTrackFields.STREAM_NOTIFICATION) {
        atStreamType = AudioSystem::NOTIFICATION;
    } else if (streamType == javaAudioTrackFields.STREAM_BLUETOOTH_SCO) {
        atStreamType = AudioSystem::BLUETOOTH_SCO;
    } else {
        LOGE("Error creating AudioTrack: unknown stream type.");
        return AUDIOTRACK_ERROR_SETUP_INVALIDSTREAMTYPE;
    }

    // check the format.
    // This function was called from Java, so we compare the format against the Java constants
    if ((audioFormat != javaAudioTrackFields.PCM16) && (audioFormat != javaAudioTrackFields.PCM8)) {
        LOGE("Error creating AudioTrack: unsupported audio format.");
        return AUDIOTRACK_ERROR_SETUP_INVALIDFORMAT;
    }
    
    // compute the frame count
    int bytesPerSample = audioFormat == javaAudioTrackFields.PCM16 ? 2 : 1;
    int format = audioFormat == javaAudioTrackFields.PCM16 ? 
            AudioSystem::PCM_16_BIT : AudioSystem::PCM_8_BIT;
    int frameCount;
    if (buffSizeInBytes == -1) {
        // compute the frame count based on the system's output frame count 
        // and the native sample rate
        frameCount = (sampleRateInHertz*afFrameCount)/afSampleRate;
    } else {
        // compute the frame count based on the specified buffer size
        frameCount = buffSizeInBytes / (nbChannels * bytesPerSample);
    }
    
    AudioTrackJniStorage* lpJniStorage = new AudioTrackJniStorage();
    
    // initialize the callback information:
    // this data will be passed with every AudioTrack callback
    jclass clazz = env->GetObjectClass(thiz);
    if (clazz == NULL) {
        LOGE("Can't find %s when setting up callback.", kClassPathName);
        delete lpJniStorage;
        return AUDIOTRACK_ERROR_SETUP_NATIVEINITFAILED;
    }
    lpJniStorage->mCallbackData.audioTrack_class = (jclass)env->NewGlobalRef(clazz);
    // we use a weak reference so the AudioTrack object can be garbage collected.
    lpJniStorage->mCallbackData.audioTrack_ref = env->NewGlobalRef(weak_this);
    
    lpJniStorage->mStreamType = atStreamType;
    
    // create the native AudioTrack object
    AudioTrack* lpTrack = new AudioTrack();
    if (lpTrack == NULL) {
        LOGE("Error creating uninitialized AudioTrack");
        goto native_track_failure;
    }
    
    // initialize the native AudioTrack object
    if (memoryMode == javaAudioTrackFields.MODE_STREAM) {

        lpTrack->set(
            atStreamType,// stream type
            sampleRateInHertz,
            format,// word length, PCM
            nbChannels,
            frameCount,
            0,// flags
            audioCallback, &(lpJniStorage->mCallbackData),//callback, callback data (user)
            0,// notificationFrames == 0 since not using EVENT_MORE_DATA to feed the AudioTrack
            0,// shared mem
            true);// thread can call Java
            
    } else if (memoryMode == javaAudioTrackFields.MODE_STATIC) {
        // AudioTrack is using shared memory
        
        if (!lpJniStorage->allocSharedMem(buffSizeInBytes)) {
            LOGE("Error creating AudioTrack in static mode: error creating mem heap base");
            goto native_init_failure;
        }
        
        lpTrack->set(
            atStreamType,// stream type
            sampleRateInHertz,
            format,// word length, PCM
            nbChannels,
            frameCount,
            0,// flags
            audioCallback, &(lpJniStorage->mCallbackData),//callback, callback data (user));
            0,// notificationFrames == 0 since not using EVENT_MORE_DATA to feed the AudioTrack 
            lpJniStorage->mMemBase,// shared mem
            true);// thread can call Java 
    }

    if (lpTrack->initCheck() != NO_ERROR) {
        LOGE("Error initializing AudioTrack");
        goto native_init_failure;
    }

    // save our newly created C++ AudioTrack in the "nativeTrackInJavaObj" field 
    // of the Java object (in mNativeTrackInJavaObj)
    env->SetIntField(thiz, javaAudioTrackFields.nativeTrackInJavaObj, (int)lpTrack);
    
    // save the JNI resources so we can free them later
    //LOGV("storing lpJniStorage: %x\n", (int)lpJniStorage);
    env->SetIntField(thiz, javaAudioTrackFields.jniData, (int)lpJniStorage);

    return AUDIOTRACK_SUCCESS;
    
    // failures:
native_init_failure:
    delete lpTrack;
    env->SetIntField(thiz, javaAudioTrackFields.nativeTrackInJavaObj, 0);
    
native_track_failure:
    delete lpJniStorage;
    env->SetIntField(thiz, javaAudioTrackFields.jniData, 0);
    return AUDIOTRACK_ERROR_SETUP_NATIVEINITFAILED;
    
}


// ----------------------------------------------------------------------------
static void
android_media_AudioTrack_start(JNIEnv *env, jobject thiz)
{
    AudioTrack *lpTrack = (AudioTrack *)env->GetIntField(
        thiz, javaAudioTrackFields.nativeTrackInJavaObj);
    if (lpTrack == NULL ) {
        jniThrowException(env, "java/lang/IllegalStateException",
            "Unable to retrieve AudioTrack pointer for start()");
    }
    
    lpTrack->start();
}


// ----------------------------------------------------------------------------
static void
android_media_AudioTrack_stop(JNIEnv *env, jobject thiz)
{
    AudioTrack *lpTrack = (AudioTrack *)env->GetIntField(
        thiz, javaAudioTrackFields.nativeTrackInJavaObj);
    if (lpTrack == NULL ) {
        jniThrowException(env, "java/lang/IllegalStateException",
            "Unable to retrieve AudioTrack pointer for stop()");
    }

    lpTrack->stop();
}


// ----------------------------------------------------------------------------
static void
android_media_AudioTrack_pause(JNIEnv *env, jobject thiz)
{
    AudioTrack *lpTrack = (AudioTrack *)env->GetIntField(
        thiz, javaAudioTrackFields.nativeTrackInJavaObj);
    if (lpTrack == NULL ) {
        jniThrowException(env, "java/lang/IllegalStateException",
            "Unable to retrieve AudioTrack pointer for pause()");
    }

    lpTrack->pause();
}


// ----------------------------------------------------------------------------
static void
android_media_AudioTrack_flush(JNIEnv *env, jobject thiz)
{
    AudioTrack *lpTrack = (AudioTrack *)env->GetIntField(
        thiz, javaAudioTrackFields.nativeTrackInJavaObj);
    if (lpTrack == NULL ) {
        jniThrowException(env, "java/lang/IllegalStateException",
            "Unable to retrieve AudioTrack pointer for flush()");
    }

    lpTrack->flush();
}

// ----------------------------------------------------------------------------
static void
android_media_AudioTrack_set_volume(JNIEnv *env, jobject thiz, jfloat leftVol, jfloat rightVol )
{
    AudioTrack *lpTrack = (AudioTrack *)env->GetIntField(
        thiz, javaAudioTrackFields.nativeTrackInJavaObj);
    if (lpTrack == NULL ) {
        jniThrowException(env, "java/lang/IllegalStateException",
            "Unable to retrieve AudioTrack pointer for setVolume()");
    }

    lpTrack->setVolume(leftVol, rightVol);
}

// ----------------------------------------------------------------------------
static void android_media_AudioTrack_native_finalize(JNIEnv *env,  jobject thiz) {
    LOGV("android_media_AudioTrack_native_finalize jobject: %x\n", (int)thiz);
       
    // delete the AudioTrack object
    AudioTrack *lpTrack = (AudioTrack *)env->GetIntField(
        thiz, javaAudioTrackFields.nativeTrackInJavaObj);
    if (lpTrack) {
        LOGV("deleting lpTrack: %x\n", (int)lpTrack);
        lpTrack->stop();
        delete lpTrack;
    }
    
    // delete the JNI data
    AudioTrackJniStorage* pJniStorage = (AudioTrackJniStorage *)env->GetIntField(
        thiz, javaAudioTrackFields.jniData);
    if (pJniStorage) {
        LOGV("deleting pJniStorage: %x\n", (int)pJniStorage);
        delete pJniStorage;
    }
}

// ----------------------------------------------------------------------------
static void android_media_AudioTrack_native_release(JNIEnv *env,  jobject thiz) {
       
    // do everything a call to finalize would
    android_media_AudioTrack_native_finalize(env, thiz);
    // + reset the native resources in the Java object so any attempt to access
    // them after a call to release fails.
    env->SetIntField(thiz, javaAudioTrackFields.nativeTrackInJavaObj, 0);
    env->SetIntField(thiz, javaAudioTrackFields.jniData, 0);
}


// ----------------------------------------------------------------------------
static jint android_media_AudioTrack_native_write(JNIEnv *env,  jobject thiz,
                                                  jbyteArray javaAudioData,
                                                  jint offsetInBytes, jint sizeInBytes) {
    jbyte* cAudioData = NULL;
    AudioTrack *lpTrack = NULL;
    //LOGV("android_media_AudioTrack_native_write(offset=%d, sizeInBytes=%d) called",
    //    offsetInBytes, sizeInBytes);
    
    // get the audio track to load with samples
    lpTrack = (AudioTrack *)env->GetIntField(thiz, javaAudioTrackFields.nativeTrackInJavaObj);
    if (lpTrack == NULL) {
        jniThrowException(env, "java/lang/IllegalStateException",
            "Unable to retrieve AudioTrack pointer for write()");
    }

    // get the pointer for the audio data from the java array
    if (javaAudioData) {
        cAudioData = (jbyte *)env->GetPrimitiveArrayCritical(javaAudioData, NULL);
        if (cAudioData == NULL) {
            LOGE("Error retrieving source of audio data to play, can't play");
            return 0; // out of memory or no data to load
        }
    } else {
        LOGE("NULL java array of audio data to play, can't play");
        return 0;
    }

    // give the data to the native AudioTrack object (the data starts at the offset)
    ssize_t written = 0;
    // regular write() or copy the data to the AudioTrack's shared memory?
    if (lpTrack->sharedBuffer() == 0) {
        written = lpTrack->write(cAudioData + offsetInBytes, sizeInBytes);
    } else {
        memcpy(lpTrack->sharedBuffer()->pointer(), cAudioData + offsetInBytes, sizeInBytes);
        written = sizeInBytes;
    }

    env->ReleasePrimitiveArrayCritical(javaAudioData, cAudioData, 0);

    //LOGV("write wrote %d (tried %d) bytes in the native AudioTrack with offset %d",
    //     (int)written, (int)(sizeInBytes), (int)offsetInBytes);
    return (int)written;
}


// ----------------------------------------------------------------------------
static jint android_media_AudioTrack_native_write_short(JNIEnv *env,  jobject thiz,
                                                  jshortArray javaAudioData,
                                                  jint offsetInShorts, jint sizeInShorts) {
    return (android_media_AudioTrack_native_write(env, thiz,
                                                 (jbyteArray) javaAudioData,
                                                 offsetInShorts*2, sizeInShorts*2)
            / 2);
}


// ----------------------------------------------------------------------------
static jint android_media_AudioTrack_get_native_frame_count(JNIEnv *env,  jobject thiz) {
    AudioTrack *lpTrack = (AudioTrack *)env->GetIntField(
        thiz, javaAudioTrackFields.nativeTrackInJavaObj);

    if (lpTrack) {
        return lpTrack->frameCount();
    } else {
        jniThrowException(env, "java/lang/IllegalStateException",
            "Unable to retrieve AudioTrack pointer for frameCount()");
        return AUDIOTRACK_ERROR;
    }
}


// ----------------------------------------------------------------------------
static void android_media_AudioTrack_set_playback_rate(JNIEnv *env,  jobject thiz,
        jint sampleRateInHz) {
    AudioTrack *lpTrack = (AudioTrack *)env->GetIntField(
                thiz, javaAudioTrackFields.nativeTrackInJavaObj);

    if (lpTrack) {
        lpTrack->setSampleRate(sampleRateInHz);   
    } else {
        jniThrowException(env, "java/lang/IllegalStateException",
            "Unable to retrieve AudioTrack pointer for setSampleRate()");
    }
}


// ----------------------------------------------------------------------------
static jint android_media_AudioTrack_get_playback_rate(JNIEnv *env,  jobject thiz) {
    AudioTrack *lpTrack = (AudioTrack *)env->GetIntField(
                thiz, javaAudioTrackFields.nativeTrackInJavaObj);

    if (lpTrack) {
        return (jint) lpTrack->getSampleRate();   
    } else {
        jniThrowException(env, "java/lang/IllegalStateException",
            "Unable to retrieve AudioTrack pointer for getSampleRate()");
        return AUDIOTRACK_ERROR;
    }
}


// ----------------------------------------------------------------------------
static jint android_media_AudioTrack_set_marker_pos(JNIEnv *env,  jobject thiz, 
        jint markerPos) {
            
    AudioTrack *lpTrack = (AudioTrack *)env->GetIntField(
                thiz, javaAudioTrackFields.nativeTrackInJavaObj);
                
    if (lpTrack) {
        return android_media_translateErrorCode( lpTrack->setMarkerPosition(markerPos) );   
    } else {
        jniThrowException(env, "java/lang/IllegalStateException",
            "Unable to retrieve AudioTrack pointer for setMarkerPosition()");
        return AUDIOTRACK_ERROR;
    }
}


// ----------------------------------------------------------------------------
static jint android_media_AudioTrack_get_marker_pos(JNIEnv *env,  jobject thiz) {
    
    AudioTrack *lpTrack = (AudioTrack *)env->GetIntField(
                thiz, javaAudioTrackFields.nativeTrackInJavaObj);
    uint32_t markerPos = 0;
                
    if (lpTrack) {
        lpTrack->getMarkerPosition(&markerPos);
        return (jint)markerPos;
    } else {
        jniThrowException(env, "java/lang/IllegalStateException",
            "Unable to retrieve AudioTrack pointer for getMarkerPosition()");
        return AUDIOTRACK_ERROR;
    }
}


// ----------------------------------------------------------------------------
static jint android_media_AudioTrack_set_pos_update_period(JNIEnv *env,  jobject thiz,
        jint period) {
            
    AudioTrack *lpTrack = (AudioTrack *)env->GetIntField(
                thiz, javaAudioTrackFields.nativeTrackInJavaObj);
                
    if (lpTrack) {
        return android_media_translateErrorCode( lpTrack->setPositionUpdatePeriod(period) );   
    } else {
        jniThrowException(env, "java/lang/IllegalStateException",
            "Unable to retrieve AudioTrack pointer for setPositionUpdatePeriod()");
        return AUDIOTRACK_ERROR;
    }            
}


// ----------------------------------------------------------------------------
static jint android_media_AudioTrack_get_pos_update_period(JNIEnv *env,  jobject thiz) {
    
    AudioTrack *lpTrack = (AudioTrack *)env->GetIntField(
                thiz, javaAudioTrackFields.nativeTrackInJavaObj);
    uint32_t period = 0;
                
    if (lpTrack) {
        lpTrack->getPositionUpdatePeriod(&period);
        return (jint)period;
    } else {
        jniThrowException(env, "java/lang/IllegalStateException",
            "Unable to retrieve AudioTrack pointer for getPositionUpdatePeriod()");
        return AUDIOTRACK_ERROR;
    }
}


// ----------------------------------------------------------------------------
static jint android_media_AudioTrack_set_position(JNIEnv *env,  jobject thiz, 
        jint position) {
            
    AudioTrack *lpTrack = (AudioTrack *)env->GetIntField(
                thiz, javaAudioTrackFields.nativeTrackInJavaObj);
                
    if (lpTrack) {
        return android_media_translateErrorCode( lpTrack->setPosition(position) );
    } else {
        jniThrowException(env, "java/lang/IllegalStateException",
            "Unable to retrieve AudioTrack pointer for setPosition()");
        return AUDIOTRACK_ERROR;
    }
}


// ----------------------------------------------------------------------------
static jint android_media_AudioTrack_get_position(JNIEnv *env,  jobject thiz) {
    
    AudioTrack *lpTrack = (AudioTrack *)env->GetIntField(
                thiz, javaAudioTrackFields.nativeTrackInJavaObj);
    uint32_t position = 0;
                
    if (lpTrack) {
        lpTrack->getPosition(&position);
        return (jint)position;
    }  else {
        jniThrowException(env, "java/lang/IllegalStateException",
            "Unable to retrieve AudioTrack pointer for getPosition()");
        return AUDIOTRACK_ERROR;
    }
}


// ----------------------------------------------------------------------------
static jint android_media_AudioTrack_set_loop(JNIEnv *env,  jobject thiz,
        jint loopStart, jint loopEnd, jint loopCount) {

     AudioTrack *lpTrack = (AudioTrack *)env->GetIntField(
                thiz, javaAudioTrackFields.nativeTrackInJavaObj);
     if (lpTrack) {
        return android_media_translateErrorCode( lpTrack->setLoop(loopStart, loopEnd, loopCount) );
     }  else {
        jniThrowException(env, "java/lang/IllegalStateException",
            "Unable to retrieve AudioTrack pointer for setLoop()");
        return AUDIOTRACK_ERROR;
    }
}


// ----------------------------------------------------------------------------
static jint android_media_AudioTrack_reload(JNIEnv *env,  jobject thiz) {

     AudioTrack *lpTrack = (AudioTrack *)env->GetIntField(
                thiz, javaAudioTrackFields.nativeTrackInJavaObj);
     if (lpTrack) {
        return android_media_translateErrorCode( lpTrack->reload() );
     } else {
        jniThrowException(env, "java/lang/IllegalStateException",
            "Unable to retrieve AudioTrack pointer for reload()");
        return AUDIOTRACK_ERROR;
    }
}


// ----------------------------------------------------------------------------
static jint android_media_AudioTrack_get_output_sample_rate(JNIEnv *env,  jobject thiz) {
    int afSamplingRate;    
    AudioTrackJniStorage* lpJniStorage = (AudioTrackJniStorage *)env->GetIntField(
        thiz, javaAudioTrackFields.jniData);
    if (lpJniStorage == NULL) {
        return DEFAULT_OUTPUT_SAMPLE_RATE;
    }

    if (AudioSystem::getOutputSamplingRate(&afSamplingRate, lpJniStorage->mStreamType) != NO_ERROR) {
        return DEFAULT_OUTPUT_SAMPLE_RATE;
    } else {
        return afSamplingRate;
    }
}


// ----------------------------------------------------------------------------
// returns the minimum required size for the successful creation of a streaming AudioTrack
static jint android_media_AudioTrack_get_min_buff_size(JNIEnv *env,  jobject thiz,
    jint sampleRateInHertz, jint nbChannels, jint audioFormat) {
    int afSamplingRate;
    int afFrameCount;
    uint32_t afLatency;
    
    if (AudioSystem::getOutputSamplingRate(&afSamplingRate) != NO_ERROR) {
        return -1;
    }
    if (AudioSystem::getOutputFrameCount(&afFrameCount) != NO_ERROR) {
        return -1;
    }
    
    if (AudioSystem::getOutputLatency(&afLatency) != NO_ERROR) {
        return -1;
    }
    
    // Ensure that buffer depth covers at least audio hardware latency
    uint32_t minBufCount = afLatency / ((1000 * afFrameCount)/afSamplingRate);
    uint32_t minFrameCount = (afFrameCount*sampleRateInHertz*minBufCount)/afSamplingRate;
    int minBuffSize = minFrameCount 
            * (audioFormat == javaAudioTrackFields.PCM16 ? 2 : 1)
            * nbChannels;
    return minBuffSize;
}


// ----------------------------------------------------------------------------
// ----------------------------------------------------------------------------
static JNINativeMethod gMethods[] = {
    // name,              signature,     funcPtr
    {"native_start",         "()V",      (void *)android_media_AudioTrack_start},
    {"native_stop",          "()V",      (void *)android_media_AudioTrack_stop},
    {"native_pause",         "()V",      (void *)android_media_AudioTrack_pause},
    {"native_flush",         "()V",      (void *)android_media_AudioTrack_flush},
    {"native_setup",         "(Ljava/lang/Object;IIIIII)I", 
                                         (void *)android_media_AudioTrack_native_setup},
    {"native_finalize",      "()V",      (void *)android_media_AudioTrack_native_finalize},
    {"native_release",       "()V",      (void *)android_media_AudioTrack_native_release},
    {"native_write_byte",    "([BII)I",  (void *)android_media_AudioTrack_native_write},
    {"native_write_short",   "([SII)I",  (void *)android_media_AudioTrack_native_write_short},
    {"native_setVolume",     "(FF)V",    (void *)android_media_AudioTrack_set_volume},
    {"native_get_native_frame_count",
                             "()I",      (void *)android_media_AudioTrack_get_native_frame_count},
    {"native_set_playback_rate",
                             "(I)V",     (void *)android_media_AudioTrack_set_playback_rate},
    {"native_get_playback_rate",
                             "()I",      (void *)android_media_AudioTrack_get_playback_rate},
    {"native_set_marker_pos","(I)I",     (void *)android_media_AudioTrack_set_marker_pos},
    {"native_get_marker_pos","()I",      (void *)android_media_AudioTrack_get_marker_pos},
    {"native_set_pos_update_period",
                             "(I)I",     (void *)android_media_AudioTrack_set_pos_update_period},
    {"native_get_pos_update_period",
                             "()I",      (void *)android_media_AudioTrack_get_pos_update_period},
    {"native_set_position",  "(I)I",     (void *)android_media_AudioTrack_set_position},
    {"native_get_position",  "()I",      (void *)android_media_AudioTrack_get_position},
    {"native_set_loop",      "(III)I",   (void *)android_media_AudioTrack_set_loop},
    {"native_reload_static", "()I",      (void *)android_media_AudioTrack_reload},
    {"native_get_output_sample_rate",
                             "()I",      (void *)android_media_AudioTrack_get_output_sample_rate},
    {"native_get_min_buff_size",
                             "(III)I",   (void *)android_media_AudioTrack_get_min_buff_size},
};


// field names found in android/media/AudioTrack.java
#define JAVA_POSTEVENT_CALLBACK_NAME                    "postEventFromNative"
#define JAVA_CONST_PCM16_NAME                           "ENCODING_PCM_16BIT"
#define JAVA_CONST_PCM8_NAME                            "ENCODING_PCM_8BIT"
#define JAVA_CONST_BUFFER_COUNT_NAME                    "BUFFER_COUNT"
#define JAVA_CONST_STREAM_VOICE_CALL_NAME               "STREAM_VOICE_CALL"
#define JAVA_CONST_STREAM_SYSTEM_NAME                   "STREAM_SYSTEM"
#define JAVA_CONST_STREAM_RING_NAME                     "STREAM_RING"
#define JAVA_CONST_STREAM_MUSIC_NAME                    "STREAM_MUSIC"
#define JAVA_CONST_STREAM_ALARM_NAME                    "STREAM_ALARM"
#define JAVA_CONST_STREAM_NOTIFICATION_NAME             "STREAM_NOTIFICATION"
#define JAVA_CONST_STREAM_BLUETOOTH_SCO_NAME            "STREAM_BLUETOOTH_SCO"
#define JAVA_CONST_MODE_STREAM_NAME                     "MODE_STREAM"
#define JAVA_CONST_MODE_STATIC_NAME                     "MODE_STATIC"
#define JAVA_NATIVETRACKINJAVAOBJ_FIELD_NAME            "mNativeTrackInJavaObj"
#define JAVA_JNIDATA_FIELD_NAME                         "mJniData"

#define JAVA_AUDIOFORMAT_CLASS_NAME             "android/media/AudioFormat"
#define JAVA_AUDIOMANAGER_CLASS_NAME            "android/media/AudioManager"

// ----------------------------------------------------------------------------
// preconditions:
//    theClass is valid
bool android_media_getIntConstantFromClass(JNIEnv* pEnv, jclass theClass, const char* className,
                             const char* constName, int* constVal) {
    jfieldID javaConst = NULL;
    javaConst = pEnv->GetStaticFieldID(theClass, constName, "I");
    if (javaConst != NULL) {
        *constVal = pEnv->GetStaticIntField(theClass, javaConst);
        return true;
    } else {
        LOGE("Can't find %s.%s", className, constName);
        return false;
    }
}


// ----------------------------------------------------------------------------
int register_android_media_AudioTrack(JNIEnv *env)
{
    javaAudioTrackFields.audioTrackClass = NULL;
    javaAudioTrackFields.nativeTrackInJavaObj = NULL;
    javaAudioTrackFields.postNativeEventInJava = NULL;

    // Get the AudioTrack class
    javaAudioTrackFields.audioTrackClass = env->FindClass(kClassPathName);
    if (javaAudioTrackFields.audioTrackClass == NULL) {
        LOGE("Can't find %s", kClassPathName);
        return -1;
    }

    // Get the postEvent method
    javaAudioTrackFields.postNativeEventInJava = env->GetStaticMethodID(
            javaAudioTrackFields.audioTrackClass,
            JAVA_POSTEVENT_CALLBACK_NAME, "(Ljava/lang/Object;IIILjava/lang/Object;)V");
    if (javaAudioTrackFields.postNativeEventInJava == NULL) {
        LOGE("Can't find AudioTrack.%s", JAVA_POSTEVENT_CALLBACK_NAME);
        return -1;
    }

    // Get the variables fields
    //      nativeTrackInJavaObj
    javaAudioTrackFields.nativeTrackInJavaObj = env->GetFieldID(
            javaAudioTrackFields.audioTrackClass,
            JAVA_NATIVETRACKINJAVAOBJ_FIELD_NAME, "I");
    if (javaAudioTrackFields.nativeTrackInJavaObj == NULL) {
        LOGE("Can't find AudioTrack.%s", JAVA_NATIVETRACKINJAVAOBJ_FIELD_NAME);
        return -1;
    }
    //      jniData;
    javaAudioTrackFields.jniData = env->GetFieldID(
            javaAudioTrackFields.audioTrackClass,
            JAVA_JNIDATA_FIELD_NAME, "I");
    if (javaAudioTrackFields.jniData == NULL) {
        LOGE("Can't find AudioTrack.%s", JAVA_JNIDATA_FIELD_NAME);
        return -1;
    }

    // Get the memory mode constants
    if ( !android_media_getIntConstantFromClass(env, javaAudioTrackFields.audioTrackClass,
               kClassPathName, 
               JAVA_CONST_MODE_STATIC_NAME, &(javaAudioTrackFields.MODE_STATIC))
         || !android_media_getIntConstantFromClass(env, javaAudioTrackFields.audioTrackClass,
               kClassPathName, 
               JAVA_CONST_MODE_STREAM_NAME, &(javaAudioTrackFields.MODE_STREAM)) ) {
        // error log performed in android_media_getIntConstantFromClass() 
        return -1;
    }

    // Get the format constants from the AudioFormat class
    jclass audioFormatClass = NULL;
    audioFormatClass = env->FindClass(JAVA_AUDIOFORMAT_CLASS_NAME);
    if (audioFormatClass == NULL) {
        LOGE("Can't find %s", JAVA_AUDIOFORMAT_CLASS_NAME);
        return -1;
    }
    if ( !android_media_getIntConstantFromClass(env, audioFormatClass, 
                JAVA_AUDIOFORMAT_CLASS_NAME, 
                JAVA_CONST_PCM16_NAME, &(javaAudioTrackFields.PCM16))
           || !android_media_getIntConstantFromClass(env, audioFormatClass, 
                JAVA_AUDIOFORMAT_CLASS_NAME, 
                JAVA_CONST_PCM8_NAME, &(javaAudioTrackFields.PCM8)) ) {
        // error log performed in android_media_getIntConstantFromClass() 
        return -1;
    }
 
    // Get the stream types from the AudioManager class
    jclass audioManagerClass = NULL;
    audioManagerClass = env->FindClass(JAVA_AUDIOMANAGER_CLASS_NAME);
    if (audioManagerClass == NULL) {
       LOGE("Can't find %s", JAVA_AUDIOMANAGER_CLASS_NAME);
       return -1;
    }
    if ( !android_media_getIntConstantFromClass(env, audioManagerClass,
               JAVA_AUDIOMANAGER_CLASS_NAME,
               JAVA_CONST_STREAM_VOICE_CALL_NAME, &(javaAudioTrackFields.STREAM_VOICE_CALL))
          || !android_media_getIntConstantFromClass(env, audioManagerClass,
               JAVA_AUDIOMANAGER_CLASS_NAME,
               JAVA_CONST_STREAM_MUSIC_NAME, &(javaAudioTrackFields.STREAM_MUSIC))
          || !android_media_getIntConstantFromClass(env, audioManagerClass,
               JAVA_AUDIOMANAGER_CLASS_NAME,
               JAVA_CONST_STREAM_SYSTEM_NAME, &(javaAudioTrackFields.STREAM_SYSTEM))
          || !android_media_getIntConstantFromClass(env, audioManagerClass,
               JAVA_AUDIOMANAGER_CLASS_NAME,
               JAVA_CONST_STREAM_RING_NAME, &(javaAudioTrackFields.STREAM_RING))
          || !android_media_getIntConstantFromClass(env, audioManagerClass,
               JAVA_AUDIOMANAGER_CLASS_NAME,
               JAVA_CONST_STREAM_ALARM_NAME, &(javaAudioTrackFields.STREAM_ALARM))
          || !android_media_getIntConstantFromClass(env, audioManagerClass,
               JAVA_AUDIOMANAGER_CLASS_NAME,
               JAVA_CONST_STREAM_NOTIFICATION_NAME, &(javaAudioTrackFields.STREAM_NOTIFICATION))
          || !android_media_getIntConstantFromClass(env, audioManagerClass,
               JAVA_AUDIOMANAGER_CLASS_NAME,
               JAVA_CONST_STREAM_BLUETOOTH_SCO_NAME,
               &(javaAudioTrackFields.STREAM_BLUETOOTH_SCO))) {
       // error log performed in android_media_getIntConstantFromClass()
       return -1;
    }

    return AndroidRuntime::registerNativeMethods(env, kClassPathName, gMethods, NELEM(gMethods));
}


// ----------------------------------------------------------------------------
