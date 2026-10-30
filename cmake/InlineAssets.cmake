# Build-time inlining of the pieces shared by both WebView2 pages: replaces
# the /*@common.css@*/ and /*@common.js@*/ markers in SRC with the contents
# of CSS and JS, writing the self-contained page to DST (which the resource
# script embeds). common.css itself carries /*@fonts.css@*/, /*@kdx-ui.css@*/ and
# /*@themes.css@*/ markers (the KDX-UI charter, copied into kdx/ by
# kdx-ui's tools/charte-sync.mjs), common.js a /*@icons.js@*/ one (generated asset,
# see tools/gen_assets.py).
# Run via `cmake -P` from a custom command, so editing any input rebuilds
# the exe.
#
# Args: -DSRC=<page.html> -DDST=<page.gen.html> -DCSS=<common.css> -DJS=<common.js>
#       -DFONTS=<kdx/fonts.css> -DKDXUI=<kdx/kdx-ui.css> -DTHEMES=<kdx/themes.css> -DFRAMECSS=<kdx/kdx-frame.css>
#       -DBRIDGE=<kdx/kdx-bridge.js> -DFRAMEJS=<kdx/kdx-frame.js> -DICONS=<icons.js>

foreach(var SRC DST CSS JS FONTS KDXUI THEMES FRAMECSS BRIDGE FRAMEJS ICONS)
    if(NOT DEFINED ${var})
        message(FATAL_ERROR "InlineAssets: missing -D${var}")
    endif()
endforeach()

file(READ "${SRC}" html)
file(READ "${CSS}" css)
file(READ "${JS}" js)
file(READ "${FONTS}" fonts)
file(READ "${KDXUI}" kdxui)
file(READ "${THEMES}" themes)
file(READ "${FRAMECSS}" framecss)
file(READ "${BRIDGE}" bridge)
file(READ "${FRAMEJS}" framejs)
file(READ "${ICONS}" icons)
string(REPLACE "/*@fonts.css@*/" "${fonts}" css "${css}")
string(REPLACE "/*@kdx-ui.css@*/" "${kdxui}" css "${css}")
string(REPLACE "/*@themes.css@*/" "${themes}" css "${css}")
string(REPLACE "/*@kdx-frame.css@*/" "${framecss}" css "${css}")
string(REPLACE "/*@kdx-bridge.js@*/" "${bridge}" js "${js}")
string(REPLACE "/*@kdx-frame.js@*/" "${framejs}" js "${js}")
string(REPLACE "/*@icons.js@*/" "${icons}" js "${js}")

string(FIND "${html}" "/*@common.css@*/" css_pos)
string(FIND "${html}" "/*@common.js@*/" js_pos)
if(css_pos EQUAL -1 OR js_pos EQUAL -1)
    message(FATAL_ERROR "InlineAssets: ${SRC} lacks a /*@common.css@*/ or /*@common.js@*/ marker")
endif()

string(REPLACE "/*@common.css@*/" "${css}" html "${html}")
string(REPLACE "/*@common.js@*/" "${js}" html "${html}")
file(WRITE "${DST}" "${html}")
