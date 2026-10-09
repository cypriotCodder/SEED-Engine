# Assembles Seed Editor.app from the built editor and player, then signs it ad hoc:
#   cmake -DEDITOR=<seed_editor> -DPLAYER=<seed_player> -DICON=<icon.jpg> -DVERSION=<x.y.z>
#         -DBUNDLE=<.../Seed Editor.app> -P editor_bundle.cmake
# The app is built beside BUNDLE and moved into place only when complete. tools/release_editor.py
# re-signs it for release; tests also use this script to make an app with another VERSION.
foreach(seed_required EDITOR PLAYER ICON VERSION BUNDLE)
    if(NOT DEFINED ${seed_required})
        message(FATAL_ERROR "editor_bundle.cmake needs -D${seed_required}=...")
    endif()
endforeach()
if(NOT VERSION MATCHES "^(0|[1-9][0-9]*)\\.(0|[1-9][0-9]*)\\.(0|[1-9][0-9]*)$")
    message(FATAL_ERROR "VERSION must be MAJOR.MINOR.PATCH, not \"${VERSION}\"")
endif()

get_filename_component(seed_parent "${BUNDLE}" DIRECTORY)
get_filename_component(seed_name "${BUNDLE}" NAME)
set(seed_partial "${seed_parent}/.${seed_name}.partial")
file(REMOVE_RECURSE "${seed_partial}")
file(MAKE_DIRECTORY "${seed_partial}/Contents/MacOS" "${seed_partial}/Contents/Resources")
# The player sits beside the editor, where Play and Export look for it.
file(COPY_FILE "${EDITOR}" "${seed_partial}/Contents/MacOS/seed_editor")
file(COPY_FILE "${PLAYER}" "${seed_partial}/Contents/MacOS/seed_player")
file(CHMOD "${seed_partial}/Contents/MacOS/seed_editor" "${seed_partial}/Contents/MacOS/seed_player"
    PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE)
execute_process(COMMAND /usr/bin/sips -s format icns "${ICON}" --out "${seed_partial}/Contents/Resources/Seed.icns"
    RESULT_VARIABLE seed_icon OUTPUT_QUIET ERROR_QUIET)
if(seed_icon EQUAL 0)
    set(seed_icon_entry "  <key>CFBundleIconFile</key><string>Seed</string>\n")
else()
    set(seed_icon_entry "")
endif()
file(WRITE "${seed_partial}/Contents/Info.plist" "<?xml version=\"1.0\" encoding=\"UTF-8\"?>
<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">
<plist version=\"1.0\">
<dict>
  <key>CFBundleName</key><string>Seed Editor</string>
  <key>CFBundleDisplayName</key><string>Seed Editor</string>
  <key>CFBundleExecutable</key><string>seed_editor</string>
  <key>CFBundleIdentifier</key><string>games.seed.editor</string>
  <key>CFBundlePackageType</key><string>APPL</string>
  <key>CFBundleShortVersionString</key><string>${VERSION}</string>
  <key>CFBundleVersion</key><string>${VERSION}</string>
${seed_icon_entry}  <key>LSMinimumSystemVersion</key><string>11.0</string>
  <key>NSHighResolutionCapable</key><true/>
</dict>
</plist>
")
execute_process(COMMAND /usr/bin/codesign --force --sign - "${seed_partial}/Contents/MacOS/seed_player"
    RESULT_VARIABLE seed_signed OUTPUT_QUIET ERROR_VARIABLE seed_sign_error)
if(seed_signed EQUAL 0)
    execute_process(COMMAND /usr/bin/codesign --force --sign - "${seed_partial}"
        RESULT_VARIABLE seed_signed OUTPUT_QUIET ERROR_VARIABLE seed_sign_error)
endif()
if(NOT seed_signed EQUAL 0)
    file(REMOVE_RECURSE "${seed_partial}")
    message(FATAL_ERROR "Could not sign ${seed_name}: ${seed_sign_error}")
endif()
file(REMOVE_RECURSE "${BUNDLE}")
file(RENAME "${seed_partial}" "${BUNDLE}")
