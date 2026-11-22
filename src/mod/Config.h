#pragma once

#include <string>
#include <string_view>

#include <pl/Config.hpp>

// Mengikuti pola template: struct ModConfig + dua fungsi pembuat JSON (dipakai config_generator)
// + spesialisasi pl::config::Schema. Config.cpp bawaan template TIDAK diubah.
namespace clange_me {

struct ModConfig {
    int version = 1;

    // --- umum ---
    bool enabled = true;       // tampilkan overlay lirik
    bool autoStart = true;     // langsung putar playlist saat game mulai
    bool loop = true;          // ulang playlist dari awal setelah lagu terakhir
    bool shuffle = false;
    std::string songsDir = ""; // kosong = <folder data mod>/songs
    float volume = 0.8f;       // 0..1
    int lyricsOffsetMs = 0;    // positif = lirik muncul lebih lambat

    // --- tampilan ---
    float fontSize = 0.055f;   // tinggi huruf, pecahan dari tinggi layar
    float positionY = 0.55f;   // 0 = tengah layar, 1 = tepi atas, -1 = tepi bawah
    float opacity = 1.0f;      // 0..1
    bool showContext = true;   // tampilkan baris sebelum/sesudah yang redup
    float lineSpacing = 1.25f;
    std::string fontFamily = "sans-serif";
    bool bold = true;
    std::string sungColor = "#FFD93D";    // bagian yang sudah dinyanyikan
    std::string unsungColor = "#FFFFFF";  // bagian yang belum
    std::string extrudeColor = "#2A1F4D"; // sisi tebal 3D
    float glow = 1.0f;                    // 0 = mati

    // --- warna & mode pesta ---
    std::string colorMode = "solid";        // solid | gradient | rainbow
    std::string gradientColor = "#FF4FD8";  // warna ujung gradien (dari sungColor ke sini)
    float rainbowSpeed = 0.3f;              // putaran pelangi per detik
    bool sparkle = false;                   // bintang kilau di sekitar teks
    float sparkleAmount = 0.5f;             // 0..1
    bool partyNotes = false;                // not musik melayang
    float notesAmount = 0.5f;               // 0..1

    // --- mode dunia (kata melayang di dunia, tetap di tempat saat kamera diputar) ---
    std::string layoutMode = "world";  // world | screen
    float worldFontSize = 0.07f;       // tinggi huruf kata, pecahan tinggi layar
    bool worldCaps = true;             // HURUF BESAR
    int worldHoldMs = 1500;            // kata bertahan setelah barisnya selesai
    float worldSpread = 1.0f;          // seberapa lebar kata tersebar
    float camSens = 0.20f;             // derajat putaran kamera per piksel geser
    bool camInvertX = false;
    bool camInvertY = false;
    float camZoneLeft = 0.30f;         // geser di sisi kiri layar (joystick) diabaikan

    // --- efek 3D ---
    float fovDeg = 50.0f;
    float depth = 2.2f;          // jarak teks dari kamera (satuan dunia virtual)
    float maxYawDeg = 16.0f;     // ayunan kiri-kanan
    float pitchDeg = 5.0f;       // ayunan atas-bawah
    float floatAmount = 0.02f;   // naik-turun melayang
    int extrudeLayers = 6;       // 0 = teks datar
    float extrudeDepth = 0.07f;  // ketebalan total
    int transitionMs = 450;      // durasi animasi pindah baris
};

nlohmann::json makeDefaultConfigJson();
nlohmann::json makeConfigSchemaJson();

} // namespace clange_me

// Nama yang dipakai di seluruh kode mod.
using LyricsConfig = clange_me::ModConfig;

#define LYR_FIELD(key, ttl, desc)                      \
    if (name == key) return {.title = ttl, .description = desc};

template <> struct pl::config::Schema<clange_me::ModConfig> {
    static constexpr std::string_view title = "Lyrics 3D Config";
    static constexpr std::string_view description = {};

    static constexpr FieldSchema field(std::string_view name) {
        if (name == "version") return {.title = "Version", .readOnly = true};

        LYR_FIELD("enabled", "Tampilkan lirik", "Matikan untuk menyembunyikan overlay lirik.")
        LYR_FIELD("autoStart", "Putar otomatis", "Langsung memutar playlist saat game dimulai.")
        LYR_FIELD("loop", "Ulangi playlist", "Kembali ke lagu pertama setelah lagu terakhir selesai.")
        LYR_FIELD("shuffle", "Acak urutan", "Putar lagu dalam urutan acak.")
        LYR_FIELD("songsDir", "Folder lagu", "Kosong = folder songs di dalam folder data mod.")
        LYR_FIELD("volume", "Volume", "0.0 sampai 1.0.")
        LYR_FIELD("lyricsOffsetMs", "Geser lirik (ms)", "Positif = lirik muncul lebih lambat, negatif = lebih cepat.")

        LYR_FIELD("fontSize", "Ukuran huruf", "Pecahan tinggi layar, mis. 0.055.")
        LYR_FIELD("positionY", "Posisi vertikal", "0 = tengah, 1 = tepi atas, -1 = tepi bawah.")
        LYR_FIELD("opacity", "Transparansi", "0.0 sampai 1.0.")
        LYR_FIELD("showContext", "Baris sebelum/sesudah", "Tampilkan baris tetangga yang redup.")
        LYR_FIELD("lineSpacing", "Jarak antar baris", "Kelipatan tinggi huruf.")
        LYR_FIELD("fontFamily", "Font", "Nama keluarga font Android, mis. sans-serif, serif, monospace.")
        LYR_FIELD("bold", "Tebal", "Pakai huruf tebal.")
        LYR_FIELD("sungColor", "Warna sudah dinyanyikan", "Format #RRGGBB.")
        LYR_FIELD("unsungColor", "Warna belum dinyanyikan", "Format #RRGGBB.")
        LYR_FIELD("extrudeColor", "Warna sisi tebal 3D", "Format #RRGGBB.")
        LYR_FIELD("glow", "Cahaya", "0 = mati, 1 = normal.")

        LYR_FIELD("colorMode", "Mode warna", "solid = satu warna, gradient = gradien, rainbow = pelangi.")
        LYR_FIELD("gradientColor", "Warna gradien", "Warna ujung gradien. Format #RRGGBB.")
        LYR_FIELD("rainbowSpeed", "Kecepatan pelangi", "Putaran warna per detik.")
        LYR_FIELD("sparkle", "Kilau", "Bintang kecil berkilau di sekitar teks.")
        LYR_FIELD("sparkleAmount", "Jumlah kilau", "0.0 sampai 1.0.")
        LYR_FIELD("partyNotes", "Not musik (mode pesta)", "Not musik melayang naik di layar.")
        LYR_FIELD("notesAmount", "Jumlah not musik", "0.0 sampai 1.0.")

        LYR_FIELD("layoutMode", "Mode tampilan", "world = kata melayang di dunia, screen = baris di layar.")
        LYR_FIELD("worldFontSize", "Ukuran kata (dunia)", "Pecahan tinggi layar.")
        LYR_FIELD("worldCaps", "Huruf besar", "Ubah kata jadi HURUF BESAR di mode dunia.")
        LYR_FIELD("worldHoldMs", "Lama kata bertahan (ms)", "Setelah barisnya selesai.")
        LYR_FIELD("worldSpread", "Sebaran kata", "1 = normal, kecil = lebih rapat.")
        LYR_FIELD("camSens", "Sensitivitas putar kamera", "Derajat per piksel geseran jari.")
        LYR_FIELD("camInvertX", "Balik arah kiri-kanan", "Aktifkan jika teks bergerak ke arah yang salah.")
        LYR_FIELD("camInvertY", "Balik arah atas-bawah", "Aktifkan jika teks bergerak ke arah yang salah.")
        LYR_FIELD("camZoneLeft", "Zona joystick (diabaikan)", "Pecahan lebar layar dari kiri.")

        LYR_FIELD("fovDeg", "Sudut pandang kamera", "Derajat.")
        LYR_FIELD("depth", "Jarak teks", "Satuan dunia virtual.")
        LYR_FIELD("maxYawDeg", "Ayunan kiri-kanan", "Derajat. 0 = diam.")
        LYR_FIELD("pitchDeg", "Ayunan atas-bawah", "Derajat. 0 = diam.")
        LYR_FIELD("floatAmount", "Efek melayang", "Seberapa naik-turun teksnya.")
        LYR_FIELD("extrudeLayers", "Lapisan ketebalan", "0 = teks datar, makin banyak makin tebal.")
        LYR_FIELD("extrudeDepth", "Ketebalan 3D", "Ketebalan total sisi teks.")
        LYR_FIELD("transitionMs", "Durasi animasi (ms)", "Kecepatan perpindahan antar baris.")
        return {};
    }
};

#undef LYR_FIELD
