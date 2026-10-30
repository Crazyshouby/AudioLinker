# Build-time inlining of the pieces of both WebView2 pages: replaces the
# /*@common.css@*/ and /*@common.js@*/ markers in SRC (the pieces shared by both
# pages) with the contents of CSS and JS, and the /*@page.css@*/ and /*@page.js@*/
# ones (the page's own styles and script, assets/<page>.css and .js) with PAGECSS
# and PAGEJS, writing the self-contained page to DST (which the resource script embeds). common.css itself carries /*@fonts.css@*/, /*@kdx-ui.css@*/ and
# /*@themes.css@*/ markers (the KDX-UI charter, copied into kdx/ by
# kdx-ui's tools/charte-sync.mjs), common.js a /*@icons.js@*/ one (generated asset,
# see tools/gen_assets.py).
# Run via `cmake -P` from a custom command, so editing any input rebuilds
# the exe.
#
# Args: -DSRC=<page.html> -DDST=<page.gen.html> -DCSS=<common.css> -DJS=<common.js>
#       -DFONTS=<kdx/fonts.css> -DKDXUI=<kdx/kdx-ui.css> -DTHEMES=<kdx/themes.css> -DFRAMECSS=<kdx/kdx-frame.css>
#       -DBRIDGE=<kdx/kdx-bridge.js> -DFRAMEJS=<kdx/kdx-frame.js> -DICONS=<icons.js>
#       -DPAGECSS=<page.css> -DPAGEJS=<page.js>

foreach(var SRC DST CSS JS FONTS KDXUI THEMES FRAMECSS BRIDGE FRAMEJS ICONS PAGECSS PAGEJS)
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
file(READ "${PAGECSS}" pagecss)
file(READ "${PAGEJS}" pagejs)
string(REPLACE "/*@fonts.css@*/" "${fonts}" css "${css}")
string(REPLACE "/*@kdx-ui.css@*/" "${kdxui}" css "${css}")
string(REPLACE "/*@themes.css@*/" "${themes}" css "${css}")
string(REPLACE "/*@kdx-frame.css@*/" "${framecss}" css "${css}")
string(REPLACE "/*@kdx-bridge.js@*/" "${bridge}" js "${js}")
string(REPLACE "/*@kdx-frame.js@*/" "${framejs}" js "${js}")
string(REPLACE "/*@icons.js@*/" "${icons}" js "${js}")

foreach(marker common.css common.js page.css page.js)
    string(FIND "${html}" "/*@${marker}@*/" marker_pos)
    if(marker_pos EQUAL -1)
        message(FATAL_ERROR "InlineAssets: ${SRC} lacks a /*@${marker}@*/ marker")
    endif()
endforeach()

string(REPLACE "/*@common.css@*/" "${css}" html "${html}")
string(REPLACE "/*@common.js@*/" "${js}" html "${html}")
string(REPLACE "/*@page.css@*/" "${pagecss}" html "${html}")
string(REPLACE "/*@page.js@*/" "${pagejs}" html "${html}")
file(WRITE "${DST}" "${html}")
