# Speed Dreams 2.4 libraries, modules and robots for Android.
#
# Layout mirrors the Linux build: a core (libsdvr.so) that exports all the
# tgf/tgfclient/tgfdata/robottools/plib/SDL/GL/AL/... symbols, plus one shared
# library per module/robot (libsd_<name>.so) that is dlopen-ed by name at runtime
# (see the ANDROID branches in src/libs/tgf/module.cpp and linuxspec.cpp).

set(SD_LIBS ${SD_SRC}/libs)
set(SD_MODS ${SD_SRC}/modules)
set(SD_DRVS ${SD_SRC}/drivers)

# --- prefixed include dirs replicating the installed layout ------------------
file(COPY ${SD_LIBS}/math/ DESTINATION ${GEN_INC}/tmath FILES_MATCHING PATTERN "*.h")
file(COPY ${SD_LIBS}/learning/ DESTINATION ${GEN_INC}/learning FILES_MATCHING PATTERN "*.h")

add_library(sd_headers INTERFACE)
target_include_directories(sd_headers INTERFACE
    ${VR_DIR}/config              # config.h for Android
    ${VR_DIR}                     # vr_state.h, vr_audio.h, vr_input.h, ... (used by the ANDROID branches)
    ${GL4ES_INCLUDE}              # GL/gl.h, GL/glext.h, GL/glu.h (GL 1.x API on GLES)
    ${GEN_INC}                    # plib/, SDL2/, cjson/, minizip/, tmath/, learning/
    ${SD_SRC}/interfaces
    ${SD_LIBS}/tgf
    ${SD_LIBS}/tgfclient
    ${SD_LIBS}/tgfdata
    ${SD_LIBS}/math
    ${SD_LIBS}/portability
    ${SD_LIBS}/robottools
    ${SD_LIBS}/txml
    ${SD_LIBS}/learning
    ${SD_MODS}/csnetworking
    ${TP_DIR}/SDL2/include        # SDL.h and friends (SDL_config.h is generated,
    $<TARGET_PROPERTY:SDL2-static,INTERFACE_INCLUDE_DIRECTORIES>   # hence also this)
    ${TP_DIR}/SDL2_ttf
    ${TP_DIR}/SDL2_mixer/include
    $<TARGET_PROPERTY:freetype,INTERFACE_INCLUDE_DIRECTORIES>
    ${OPENAL_INCLUDE}
    ${CURL_INCLUDE}
    ${TP_DIR}/rhash/librhash      # rhash.h (sha256 for the download manager)
    ${TP_DIR}/enet/include    # NOT .../include/enet: it holds time.h and list.h,
                              # which would shadow the libc / libc++ headers
    ${JPEG_INCLUDE}
    ${PNG_DIR} ${PNG_GEN}
    ${ZLIB_DIR}
    ${SD_ROOT}/freesolid/include
)
target_compile_definitions(sd_headers INTERFACE
    HAVE_CONFIG_H ANDROID _DEFAULT_SOURCE SHM TRACE_OUT TRACE_LEVEL=5
    CLIENT_SERVER SPEED_DREAMS GL_GLEXT_PROTOTYPES CURL_STATICLIB AL_LIBTYPE_STATIC)
target_compile_options(sd_headers INTERFACE
    -fno-strict-aliasing -Wno-write-strings -Wno-narrowing -Wno-register
    -Wno-deprecated-declarations -Wno-format-security -Wno-error=format-security
    -Wno-unused-result -Wno-format -Wno-deprecated-register -Wno-c++11-narrowing
    -Wno-int-to-pointer-cast -Wno-pointer-to-int-cast -Wno-unused-value
    -Wno-deprecated-non-prototype -Wno-implicit-function-declaration -Wno-int-conversion
    -Wno-unused-variable -Wno-unused-but-set-variable -Wno-unused-private-field
    -Wno-inconsistent-missing-override -Wno-overloaded-virtual -Wno-unused-function
    -Wno-sign-compare -Wno-reorder-ctor -Wno-parentheses -Wno-misleading-indentation
    -Wno-unused-lambda-capture -Wno-deprecated-copy -Wno-vla-cxx-extension)

function(sd_static_lib name dir)
    set(srcs "")
    foreach(f ${ARGN})
        list(APPEND srcs ${dir}/${f})
    endforeach()
    add_library(${name} STATIC ${srcs})
    target_link_libraries(${name} PUBLIC sd_headers)
    # SDL2 copies its public headers into the build tree as part of its own
    # build; without this dependency a source file can be compiled before
    # SDL_config.h exists.
    add_dependencies(${name} sdl_headers_copy)
endfunction()

# ------------------------------------------------------------------ portability
sd_static_lib(portability ${SD_LIBS}/portability portability.cpp posix/rand.cpp posix/rmdir_r.cpp)

# ------------------------------------------------------------------ txml (bundled expat)
sd_static_lib(txml ${SD_LIBS}/txml xmlparse.c xmltok.c xmlrole.c xml.cpp)

# ------------------------------------------------------------------ tgf
sd_static_lib(tgf ${SD_LIBS}/tgf
    application.cpp eventloop.cpp directory.cpp file.cpp filesetup.cpp
    formula.cpp hash.cpp module.cpp legacymodule.cpp modinfo.cpp os.cpp
    params.cpp profiler.cpp randname.cpp schedulespy.cpp tmppath.cpp
    tgf.cpp trace.cpp memmanager.cpp linuxspec.cpp)

# ------------------------------------------------------------------ tgfclient
sd_static_lib(tgfclient ${SD_LIBS}/tgfclient
    control.cpp glfeatures.cpp guibutton.cpp guifont.cpp
    guiimage.cpp guimenu.cpp guiscrollbar.cpp guitexture.cpp
    tgfclient.cpp gui.cpp guiedit.cpp guihelp.cpp
    guilabel.cpp guiobject.cpp guiscrollist.cpp
    guicombobox.cpp guicheckbox.cpp guiprogresbar.cpp
    guiscreen.cpp guieventloop.cpp guiapplication.cpp
    guigallery.cpp guigalleryelm.cpp
    notification.cpp musicplayer.cpp playlist.cpp
    forcefeedback.cpp guimenusfx.cpp webserver.cpp)

# ------------------------------------------------------------------ tgfdata
sd_static_lib(tgfdata ${SD_LIBS}/tgfdata
    tgfdata.cpp cars.cpp tracks.cpp racemanagers.cpp race.cpp drivers.cpp names.cpp nation.cpp)

# ------------------------------------------------------------------ robottools
sd_static_lib(robottools ${SD_LIBS}/robottools
    rtteammanager.cpp rttelem.cpp rttimeanalysis.cpp rttrack.cpp rtutil.cpp rthumandriver.cpp)

# ------------------------------------------------------------------ learning
sd_static_lib(learning ${SD_LIBS}/learning
    ANN.cpp Distribution.cpp List.cpp MathFunctions.cpp ann_policy.cpp policy.cpp string_utils.cpp)

# ------------------------------------------------------------------ csnetworking (client/server networking)
sd_static_lib(csnetworking ${SD_MODS}/csnetworking
    csnetwork.cpp csserver.cpp csclient.cpp csrobotxml.cpp cspack.cpp csstats.cpp csadhoc.cpp)

# All static pieces that make up the core. They are linked with --whole-archive so
# that every symbol is available to the dlopen-ed modules.
set(SD_CORE_LIBS
    tgfclient tgfdata robottools learning csnetworking tgf txml portability
    plib_ssgaux plib_ssg plib_sg plib_ul
    SDL2_ttf SDL2_mixer SDL2-static freetype jpeg-static png minizip zlib
    cjson rhash enet libcurl_static)

# ------------------------------------------------------------------ modules / robots
# Each becomes libsd_<name>.so.
function(sd_module name dir)
    set(srcs "")
    foreach(f ${ARGN})
        list(APPEND srcs ${dir}/${f})
    endforeach()
    add_library(sd_${name} SHARED ${srcs})
    target_link_libraries(sd_${name} PRIVATE sd_headers sdvr)
    target_link_options(sd_${name} PRIVATE -Wl,-z,max-page-size=16384)
    set_target_properties(sd_${name} PROPERTIES OUTPUT_NAME sd_${name})
    add_dependencies(sd_${name} sdl_headers_copy)
    set_property(GLOBAL APPEND PROPERTY SD_MODULE_TARGETS sd_${name})
endfunction()

sd_module(trackv1 ${SD_MODS}/track/trackv1 track.cpp track3.cpp track4.cpp track5.cpp trackitf.cpp trackutil.cpp)

sd_module(simuv5 ${SD_MODS}/simu/simuv5
    aero.cpp atmosphere.cpp axle.cpp brake.cpp car.cpp categories.cpp
    collide.cpp differential.cpp engine.cpp simu.cpp simulationOptions.cpp
    steer.cpp susp.cpp transmission.cpp wheel.cpp simuv5.cpp)
target_link_libraries(sd_simuv5 PRIVATE solid)

sd_module(telemetryv1 ${SD_MODS}/telemetry/telemetryv1 telemetryitf.cpp telemetry.cpp)

sd_module(ssggraph ${SD_MODS}/graphic/ssggraph
    grMoonpos.cpp grMoon.cpp grSun.cpp grCloudLayer.cpp grSky.cpp
    grSkyDome.cpp grSphere.cpp grStars.cpp grboard.cpp grcam.cpp grcar.cpp
    grcarlight.cpp grmain.cpp grsimplestate.cpp grmultitexstate.cpp grloadac.cpp
    grscene.cpp grbackground.cpp grscreen.cpp grshadow.cpp grskidmarks.cpp
    grsmoke.cpp grtexture.cpp grtracklight.cpp grtrackmap.cpp grutil.cpp
    grvtxtable.cpp grrain.cpp ssggraph.cpp)

# snddefault without the plib (sl) backend: OpenAL only on Quest.
sd_module(snddefault ${SD_MODS}/sound/snddefault
    snddefault.cpp CarSoundData.cpp Sound.cpp OpenalSound.cpp SoundInterface.cpp
    OpenalSoundInterface.cpp grsound.cpp)

sd_module(standardgame ${SD_MODS}/racing/standardgame
    standardgame.cpp raceupdate.cpp racenetwork.cpp racecars.cpp
    raceinit.cpp racemain.cpp racetrack.cpp raceresults.cpp racesimusimu.cpp
    racestate.cpp racesituation.cpp racemessage.cpp raceutil.cpp racewebmetar.cpp)

set(LM ${SD_MODS}/userinterface/legacymenu)
sd_module(legacymenu ${LM}
    mainscreens/splash.cpp mainscreens/mainmenu.cpp mainscreens/optionsmenu.cpp
    mainscreens/creditsmenu.cpp mainscreens/exitmenu.cpp mainscreens/asset.cpp
    mainscreens/assets.cpp mainscreens/confirmmenu.cpp mainscreens/dispatcher.cpp
    mainscreens/downloadservers.cpp mainscreens/downloadsmenu.cpp mainscreens/leaderboardmenu.cpp mainscreens/entry.cpp
    mainscreens/infomenu.cpp mainscreens/ptransfer.cpp mainscreens/repomenu.cpp
    mainscreens/sha256.cpp mainscreens/sink.cpp mainscreens/thumbnail.cpp
    mainscreens/transfer.cpp mainscreens/translatable.cpp mainscreens/unzip.cpp
    mainscreens/welcomemenu.cpp mainscreens/writebuf.cpp mainscreens/writefile.cpp
    confscreens/playerconfig.cpp confscreens/controlconfig.cpp
    confscreens/joystickconfig.cpp confscreens/mouseconfig.cpp confscreens/joy2butconfig.cpp
    confscreens/serverconfig.cpp confscreens/displayconfig.cpp confscreens/graphicsconfig.cpp
    confscreens/graphinfo.cpp confscreens/monitorconfig.cpp confscreens/osggraphinfo.cpp
    confscreens/simuconfig.cpp confscreens/soundconfig.cpp confscreens/ssggraphinfo.cpp
    confscreens/hostsettingsmenu.cpp confscreens/forcefeedbackconfig.cpp confscreens/languageconfig.cpp
    racescreens/humanselect.cpp racescreens/raceselectmenu.cpp racescreens/racemanmenu.cpp
    racescreens/fileselect.cpp racescreens/raceconfigstate.cpp racescreens/trackselect.cpp
    racescreens/driverselect.cpp racescreens/garagemenu.cpp racescreens/raceparamsmenu.cpp
    racescreens/carsetupmenu.cpp racescreens/raceloadingmenu.cpp racescreens/racerunningmenus.cpp
    racescreens/raceoptimizationmenu.cpp racescreens/racestopmenu.cpp racescreens/racestartmenu.cpp
    racescreens/racepitmenu.cpp racescreens/raceresultsmenus.cpp racescreens/racenexteventmenu.cpp
    racescreens/csnetworkingmenu.cpp racescreens/csnetclientsettings.cpp racescreens/csnetserversettings.cpp
    legacymenu.cpp)
target_include_directories(sd_legacymenu PRIVATE ${LM} ${LM}/mainscreens ${LM}/confscreens ${LM}/racescreens)

# ------------------------------------------------------------------ robots
function(sd_robot name)
    set(srcs "")
    foreach(f ${ARGN})
        list(APPEND srcs ${SD_DRVS}/${name}/${f})
    endforeach()
    add_library(sd_${name} SHARED ${srcs})
    target_link_libraries(sd_${name} PRIVATE sd_headers sdvr)
    target_include_directories(sd_${name} PRIVATE ${SD_ROOT})
    target_link_options(sd_${name} PRIVATE -Wl,-z,max-page-size=16384)
    set_target_properties(sd_${name} PROPERTIES OUTPUT_NAME sd_${name})
    add_dependencies(sd_${name} sdl_headers_copy)
    set_property(GLOBAL APPEND PROPERTY SD_MODULE_TARGETS sd_${name})
endfunction()

sd_robot(human human.cpp)
sd_robot(networkhuman networkhuman.cpp)

sd_robot(simplix
    src/unitcarparam.cpp src/unitcharacteristic.cpp src/unitclothoid.cpp src/unitcollision.cpp
    src/unitcommon.cpp src/unitcubic.cpp src/unitcubicspline.cpp src/unitdriver.cpp
    src/unitfixcarparam.cpp src/unitlane.cpp src/unitlanepoint.cpp src/unitlinalg.cpp
    src/unitlinreg.cpp src/unitmain.cpp src/unitopponent.cpp src/unitparabel.cpp
    src/unitparam.cpp src/unitpidctrl.cpp src/unitpit.cpp src/unitpitparam.cpp
    src/unitsection.cpp src/unitstrategy.cpp src/unitsysfoo.cpp src/unittmpcarparam.cpp
    src/unittrack.cpp src/unitvec2d.cpp src/unitvec3d.cpp)

sd_robot(usr
    src/usr.cpp src/cubic.cpp src/datalog.cpp src/driver.cpp src/filter.cpp src/MuFactors.cpp
    src/MyCar.cpp src/MyTrack.cpp src/opponent.cpp src/opponents.cpp src/Path.cpp
    src/PathMargins.cpp src/PathState.cpp src/pidcontroller.cpp src/pit.cpp src/spline.cpp
    src/tires.cpp src/Utils.cpp)

sd_robot(axiom
    src/axiom.cpp src/CarParams.cpp src/cubic.cpp src/datalog.cpp src/driver.cpp src/filter.cpp
    src/MuFactors.cpp src/MyParam.cpp src/MyTrack.cpp src/opponent.cpp src/opponents.cpp
    src/Path.cpp src/PathMargins.cpp src/PathState.cpp src/pidcontroller.cpp src/pit.cpp
    src/spline.cpp src/Utils.cpp src/Wheels.cpp)

sd_robot(shadow
    src/Avoidance.cpp src/CarBounds2d.cpp src/CarModel.cpp src/ClothoidPath.cpp src/Cubic.cpp
    src/CubicSpline.cpp src/LearnedGraph.cpp src/LinearRegression.cpp src/Driver.cpp
    src/MyTrack.cpp src/Opponent.cpp src/ParametricCubic.cpp src/ParametricCubicSpline.cpp
    src/Path.cpp src/PathOffsets.cpp src/PathOptions.cpp src/PathRecord.cpp src/PidController.cpp
    src/PitPath.cpp src/PtInfo.cpp src/Quadratic.cpp src/Seg.cpp src/Shadow.cpp src/Shared.cpp
    src/Span.cpp src/SpringsPath.cpp src/Strategy.cpp src/Stuck.cpp src/TeamInfo.cpp src/Utils.cpp
    src/WheelModel.cpp)

sd_robot(urbanski urbanski.cpp driver.cpp geoutil.cpp trackproc.cpp)

sd_robot(dandroid
    src/ClothoidPath.cpp src/cubic.cpp src/dandroid.cpp src/danpath.cpp src/driver.cpp
    src/LinePath.cpp src/MyTrack.cpp src/opponent.cpp src/pidcontroller.cpp src/pit.cpp
    src/spline.cpp src/Utils.cpp)
