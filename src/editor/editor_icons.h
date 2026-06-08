#pragma once
// ── Editor Icon Defines ─────────────────────────────────────────────────────
// Font Awesome 6 Free Solid icon codepoints, UTF-8 encoded.
// Merged into the ImGui font atlas alongside Roboto in imguiInit().
//
// Usage: ImGui::Begin(ICON_FA_WRENCH " Inspector");
//        ImGui::Text(ICON_FA_CUBE " %s", meshName);
//
// Codepoints from github.com/juliettef/IconFontCppHeaders (MIT license).
// Font from github.com/FortAwesome/Font-Awesome (SIL OFL 1.1 + MIT).

// Glyph range for font atlas merge (start, end)
#define ICON_MIN_FA  0xe005
#define ICON_MAX_FA  0xf8ff

// ── Panel titles ────────────────────────────────────────────────────────────
#define ICON_FA_WRENCH              "\xef\x82\xad"  // U+f0ad  wrench
#define ICON_FA_SCREWDRIVER_WRENCH  "\xef\x9f\x99"  // U+f7d9  screwdriver-wrench
#define ICON_FA_SITEMAP             "\xef\x83\xa8"  // U+f0e8  sitemap (hierarchy)
#define ICON_FA_TERMINAL            "\xef\x84\xa0"  // U+f120  terminal (console)
#define ICON_FA_EYE                 "\xef\x81\xae"  // U+f06e  eye (scene view)
#define ICON_FA_GAMEPAD             "\xef\x84\x9b"  // U+f11b  gamepad (game view)
#define ICON_FA_CHART_LINE          "\xef\x88\x81"  // U+f201  chart-line (stats)
#define ICON_FA_FOLDER_OPEN         "\xef\x81\xbc"  // U+f07c  folder-open (asset browser)
#define ICON_FA_GEAR                "\xef\x80\x93"  // U+f013  gear (settings)
#define ICON_FA_SLIDERS             "\xef\x87\x9e"  // U+f1de  sliders (settings alt)

// ── Sim controls ────────────────────────────────────────────────────────────
#define ICON_FA_PLAY                "\xef\x81\x8b"  // U+f04b
#define ICON_FA_PAUSE               "\xef\x81\x8c"  // U+f04c
#define ICON_FA_STOP                "\xef\x81\x8d"  // U+f04d
#define ICON_FA_CIRCLE_PLAY         "\xef\x85\x84"  // U+f144
#define ICON_FA_CIRCLE_PAUSE        "\xef\x8a\x8b"  // U+f28b
#define ICON_FA_CIRCLE_STOP         "\xef\x8a\x8d"  // U+f28d

// ── Components / entities ───────────────────────────────────────────────────
#define ICON_FA_CUBE                "\xef\x86\xb2"  // U+f1b2  mesh / entity
#define ICON_FA_CUBES               "\xef\x86\xb3"  // U+f1b3  scene / multiple
#define ICON_FA_CAMERA              "\xef\x80\xb0"  // U+f030  camera component
#define ICON_FA_LIGHTBULB           "\xef\x83\xab"  // U+f0eb  light
#define ICON_FA_SUN                 "\xef\x86\x85"  // U+f185  directional light
#define ICON_FA_CODE                "\xef\x84\xa1"  // U+f121  script
#define ICON_FA_IMAGE               "\xef\x80\xbe"  // U+f03e  texture
#define ICON_FA_LAYER_GROUP         "\xef\x97\xbd"  // U+f5fd  materials / layers
#define ICON_FA_OBJECT_GROUP        "\xef\x89\x87"  // U+f247  group

// ── Inspector sections ──────────────────────────────────────────────────────
#define ICON_FA_ARROWS_UP_DOWN_LEFT_RIGHT "\xef\x81\x87"  // U+f047  transform
#define ICON_FA_SHAPES              "\xef\x98\x9f"  // U+f61f  rigid body / collider
#define ICON_FA_PERSON_RUNNING      "\xef\x9c\x8c"  // U+f70c  character controller

// ── Actions / misc ──────────────────────────────────────────────────────────
#define ICON_FA_FLOPPY_DISK         "\xef\x83\x87"  // U+f0c7  save
#define ICON_FA_MAGNIFYING_GLASS    "\xef\x80\x82"  // U+f002  search
#define ICON_FA_PLUS                "\x2b"          // U+002b  add
#define ICON_FA_TRASH               "\xef\x87\xb8"  // U+f1f8  delete
#define ICON_FA_FILE                "\xef\x85\x9b"  // U+f15b  file
#define ICON_FA_FOLDER              "\xef\x81\xbb"  // U+f07b  folder
#define ICON_FA_CIRCLE_INFO         "\xef\x81\x9a"  // U+f05a  info
#define ICON_FA_CIRCLE_CHECK        "\xef\x81\x98"  // U+f058  success
#define ICON_FA_TRIANGLE_EXCLAMATION "\xef\x81\xb1" // U+f071  warning
#define ICON_FA_CIRCLE_XMARK       "\xef\x81\x97"  // U+f057  error
#define ICON_FA_BUG                 "\xef\x86\x88"  // U+f188  debug
#define ICON_FA_BARS                "\xef\x83\x89"  // U+f0c9  menu
#define ICON_FA_ROTATE              "\xef\x8b\xb1"  // U+f2f1  refresh
#define ICON_FA_MOUNTAIN_SUN        "\xee\x94\xaf"  // U+e52f  scene/environment
