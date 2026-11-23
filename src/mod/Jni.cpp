#include "Jni.h"

#include "Log.h"

namespace lyrics::jni {

static JavaVM* gVm = nullptr;

void setJavaVM(JavaVM* vm) { gVm = vm; }
JavaVM* javaVM() { return gVm; }

JNIEnv* env(bool* attachedByUs) {
    if (attachedByUs) *attachedByUs = false;
    if (!gVm) return nullptr;
    JNIEnv* e = nullptr;
    jint r = gVm->GetEnv(reinterpret_cast<void**>(&e), JNI_VERSION_1_6);
    if (r == JNI_OK) return e;
    if (r == JNI_EDETACHED) {
#if defined(__ANDROID__)
        if (gVm->AttachCurrentThread(&e, nullptr) == JNI_OK) {
#else
        if (gVm->AttachCurrentThread(reinterpret_cast<void**>(&e), nullptr) == JNI_OK) {
#endif
            if (attachedByUs) *attachedByUs = true;
            return e;
        }
    }
    return nullptr;
}

void detachThread() {
    if (gVm) gVm->DetachCurrentThread();
}

bool clearException(JNIEnv* e, const char* where) {
    if (!e || !e->ExceptionCheck()) return false;
    LYR_LOGW("Java exception in %s", where ? where : "?");
    e->ExceptionDescribe();
    e->ExceptionClear();
    return true;
}

} // namespace lyrics::jni
