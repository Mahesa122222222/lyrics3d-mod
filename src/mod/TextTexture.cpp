#include "TextTexture.h"

#include <algorithm>
#include <cmath>

#include "Jni.h"
#include "Log.h"
#include "Utf8.h"

namespace lyrics {

namespace {

jclass findGlobal(JNIEnv* e, const char* name) {
    jclass local = e->FindClass(name);
    if (!local) {
        jni::clearException(e, name);
        return nullptr;
    }
    jclass g = static_cast<jclass>(e->NewGlobalRef(local));
    e->DeleteLocalRef(local);
    return g;
}

} // namespace

TextFactory::~TextFactory() = default;

bool TextFactory::init(JNIEnv* e) {
    if (mReady) return true;
    if (!e) return false;

    cPaint = findGlobal(e, "android/graphics/Paint");
    cCanvas = findGlobal(e, "android/graphics/Canvas");
    cBitmap = findGlobal(e, "android/graphics/Bitmap");
    cConfig = findGlobal(e, "android/graphics/Bitmap$Config");
    cTypeface = findGlobal(e, "android/graphics/Typeface");
    cGlUtils = findGlobal(e, "android/opengl/GLUtils");
    cMetrics = findGlobal(e, "android/graphics/Paint$FontMetrics");
    if (!cPaint || !cCanvas || !cBitmap || !cConfig || !cTypeface || !cGlUtils || !cMetrics) {
        LYR_LOGE("TextFactory: Android classes not found");
        return false;
    }

    mPaintCtor = e->GetMethodID(cPaint, "<init>", "(I)V");
    mSetTextSize = e->GetMethodID(cPaint, "setTextSize", "(F)V");
    mSetColor = e->GetMethodID(cPaint, "setColor", "(I)V");
    mSetTypeface = e->GetMethodID(cPaint, "setTypeface", "(Landroid/graphics/Typeface;)Landroid/graphics/Typeface;");
    mMeasureText = e->GetMethodID(cPaint, "measureText", "(Ljava/lang/String;)F");
    mGetFontMetrics = e->GetMethodID(cPaint, "getFontMetrics", "()Landroid/graphics/Paint$FontMetrics;");
    mSetShadow = e->GetMethodID(cPaint, "setShadowLayer", "(FFFI)V");
    mCanvasCtor = e->GetMethodID(cCanvas, "<init>", "(Landroid/graphics/Bitmap;)V");
    mDrawText = e->GetMethodID(cCanvas, "drawText", "(Ljava/lang/String;FFLandroid/graphics/Paint;)V");
    mCreateBitmap = e->GetStaticMethodID(cBitmap, "createBitmap",
                                         "(IILandroid/graphics/Bitmap$Config;)Landroid/graphics/Bitmap;");
    mRecycle = e->GetMethodID(cBitmap, "recycle", "()V");
    mTypefaceCreate = e->GetStaticMethodID(cTypeface, "create",
                                           "(Ljava/lang/String;I)Landroid/graphics/Typeface;");
    mTexImage2D = e->GetStaticMethodID(cGlUtils, "texImage2D", "(IILandroid/graphics/Bitmap;I)V");
    fAscent = e->GetFieldID(cMetrics, "ascent", "F");
    fDescent = e->GetFieldID(cMetrics, "descent", "F");

    jfieldID fArgb = e->GetStaticFieldID(cConfig, "ARGB_8888", "Landroid/graphics/Bitmap$Config;");
    if (jni::clearException(e, "TextFactory.init lookup") || !fArgb) return false;
    jobject cfg = e->GetStaticObjectField(cConfig, fArgb);
    if (!cfg) return false;
    oArgb8888 = e->NewGlobalRef(cfg);
    e->DeleteLocalRef(cfg);

    mReady = true;
    return true;
}

void TextFactory::shutdown(JNIEnv* e) {
    if (!e) return;
    auto drop = [&](jclass& c) {
        if (c) e->DeleteGlobalRef(c);
        c = nullptr;
    };
    drop(cPaint);
    drop(cCanvas);
    drop(cBitmap);
    drop(cConfig);
    drop(cTypeface);
    drop(cGlUtils);
    drop(cMetrics);
    if (oArgb8888) e->DeleteGlobalRef(oArgb8888);
    oArgb8888 = nullptr;
    mReady = false;
}

bool TextFactory::make(JNIEnv* e, const std::string& text, const TextStyle& style, TextTexture& out) {
    if (!mReady || text.empty()) return false;

    bool ok = false;
    e->PushLocalFrame(32);

    std::u16string u16 = utf8ToUtf16(text);
    jstring jtext = e->NewString(reinterpret_cast<const jchar*>(u16.data()), static_cast<jsize>(u16.size()));

    jobject paint = e->NewObject(cPaint, mPaintCtor, 1 /* ANTI_ALIAS_FLAG */);
    std::u16string fam = utf8ToUtf16(style.family);
    jstring jfam = e->NewString(reinterpret_cast<const jchar*>(fam.data()), static_cast<jsize>(fam.size()));
    jobject typeface = e->CallStaticObjectMethod(cTypeface, mTypefaceCreate, jfam, style.bold ? 1 : 0);
    if (jni::clearException(e, "Paint/Typeface") || !paint) {
        e->PopLocalFrame(nullptr);
        return false;
    }
    e->CallObjectMethod(paint, mSetTypeface, typeface);
    e->CallVoidMethod(paint, mSetColor, static_cast<jint>(0xFFFFFFFF));

    float fontPx = std::max(8.0f, style.fontPx);
    const int pad = static_cast<int>(std::ceil(style.glowRadiusPx * 2.0f)) + 4;
    const int maxTex = 4096;
    const int maxW = std::min(style.maxWidthPx, maxTex);

    e->CallVoidMethod(paint, mSetTextSize, fontPx);
    float width = e->CallFloatMethod(paint, mMeasureText, jtext);
    if (width + 2 * pad > maxW && width > 0) {
        fontPx *= static_cast<float>(maxW - 2 * pad) / width;
        fontPx = std::max(fontPx, 8.0f);
        e->CallVoidMethod(paint, mSetTextSize, fontPx);
        width = e->CallFloatMethod(paint, mMeasureText, jtext);
    }
    if (style.glowRadiusPx > 0.5f) {
        e->CallVoidMethod(paint, mSetShadow, style.glowRadiusPx, 0.0f, 0.0f, static_cast<jint>(0xFFFFFFFF));
    }

    jobject fm = e->CallObjectMethod(paint, mGetFontMetrics);
    if (jni::clearException(e, "measure") || !fm) {
        e->PopLocalFrame(nullptr);
        return false;
    }
    const float ascent = e->GetFloatField(fm, fAscent);   // negative
    const float descent = e->GetFloatField(fm, fDescent); // positive

    const int w = std::min(maxTex, static_cast<int>(std::ceil(width)) + 2 * pad);
    const int h = std::min(maxTex, static_cast<int>(std::ceil(descent - ascent)) + 2 * pad);
    if (w <= 2 * pad || h <= 2 * pad) {
        e->PopLocalFrame(nullptr);
        return false;
    }

    jobject bmp = e->CallStaticObjectMethod(cBitmap, mCreateBitmap, w, h, oArgb8888);
    if (jni::clearException(e, "createBitmap") || !bmp) {
        e->PopLocalFrame(nullptr);
        return false;
    }
    jobject canvas = e->NewObject(cCanvas, mCanvasCtor, bmp);
    e->CallVoidMethod(canvas, mDrawText, jtext, static_cast<float>(pad), static_cast<float>(pad) - ascent, paint);
    if (!jni::clearException(e, "drawText")) {
        GLuint tex = 0;
        glGenTextures(1, &tex);
        glBindTexture(GL_TEXTURE_2D, tex);
        e->CallStaticVoidMethod(cGlUtils, mTexImage2D, static_cast<jint>(GL_TEXTURE_2D), 0, bmp, 0);
        if (!jni::clearException(e, "GLUtils.texImage2D")) {
            glGenerateMipmap(GL_TEXTURE_2D);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            out.id = tex;
            out.width = w;
            out.height = h;
            out.u0 = static_cast<float>(pad) / static_cast<float>(w);
            out.u1 = static_cast<float>(w - pad) / static_cast<float>(w);
            ok = true;
        } else {
            glDeleteTextures(1, &tex);
        }
    }
    e->CallVoidMethod(bmp, mRecycle);
    jni::clearException(e, "recycle");

    e->PopLocalFrame(nullptr);
    return ok;
}

} // namespace lyrics
