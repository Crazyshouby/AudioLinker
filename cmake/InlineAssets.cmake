# Build-time inlining of the pieces shared by both WebView2 pages: replaces
# the /*@common.css@*/ and /*@common.js@*/ markers in SRC with the contents
# of CSS and JS, writing the self-contained page to DST (which the resource
# script embeds). Run via `cmake -P` from a custom command, so editing any
# of the three inputs rebuilds the exe.
#
# Args: -DSRC=<page.html> -DDST=<page.gen.html> -DCSS=<common.css> -DJS=<common.js>

foreach(var SRC DST CSS JS)
    if(NOT DEFINED ${var})
        message(FATAL_ERROR "InlineAssets: missing -D${var}")
    endif()
endforeach()

file(READ "${SRC}" html)
file(READ "${CSS}" css)
file(READ "${JS}" js)

string(FIND "${html}" "/*@common.css@*/" css_pos)
string(FIND "${html}" "/*@common.js@*/" js_pos)
if(css_pos EQUAL -1 OR js_pos EQUAL -1)
    message(FATAL_ERROR "InlineAssets: ${SRC} lacks a /*@common.css@*/ or /*@common.js@*/ marker")
endif()

string(REPLACE "/*@common.css@*/" "${css}" html "${html}")
string(REPLACE "/*@common.js@*/" "${js}" html "${html}")
file(WRITE "${DST}" "${html}")
