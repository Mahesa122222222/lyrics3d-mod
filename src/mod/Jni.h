#pragma once
#include <jni.h>

namespace lyrics::jni {

void setJavaVM(JavaVM* vm);
JavaVM* javaVM();

// Ambil JNIEnv untuk thread saat ini; attach kalau perlu. nullptr jika gagal.
// Thread yang kita attach sendiri (mis. thread audio) harus memanggil detachThread() di akhir.
JNIEnv* env(bool* attachedByUs = nullptr);
void detachThread();

// true jika ada exception Java; exception dibersihkan dan dicatat ke log.
bool clearException(JNIEnv* e, const char* where);

} // namespace lyrics::jni
