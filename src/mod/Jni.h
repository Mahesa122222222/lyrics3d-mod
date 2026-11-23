#pragma once
#include <jni.h>

namespace lyrics::jni {

void setJavaVM(JavaVM* vm);
JavaVM* javaVM();

// Get the JNIEnv for the current thread; attach if needed. nullptr on failure.
// Threads we attach ourselves (e.g. the audio thread) must call detachThread() at the end.
JNIEnv* env(bool* attachedByUs = nullptr);
void detachThread();

// true if there is a Java exception; the exception is cleared and logged.
bool clearException(JNIEnv* e, const char* where);

} // namespace lyrics::jni
