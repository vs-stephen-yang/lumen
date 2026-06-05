# Deploy the sender web root next to web_receiver.exe (run POST_BUILD, in
# script mode). Prefers the productionized Vite build (web/dist) when present,
# falling back to the legacy inline test page. Existence is checked at build
# time, so building web/ (`npm run build`) and rebuilding the receiver swaps in
# the real app with no reconfigure.
#
# Args (via -D): WEB_DIST, TEST_PAGE, OUT_DIR
if(EXISTS "${WEB_DIST}/index.html")
    message(STATUS "web_receiver: serving productionized web/dist")
    file(REMOVE_RECURSE "${OUT_DIR}")
    file(COPY "${WEB_DIST}/" DESTINATION "${OUT_DIR}")
else()
    message(STATUS "web_receiver: serving legacy test_page (web/dist not built)")
    file(REMOVE_RECURSE "${OUT_DIR}")
    file(COPY "${TEST_PAGE}/" DESTINATION "${OUT_DIR}")
endif()
