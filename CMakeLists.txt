project(ib-sample)

if (WIN32)
    # Prevent include WinSock.h
    add_definitions(-DWIN32_LEAN_AND_MEAN -D_WIN32_WINNT=0x0601 -D_SCL_SECURE_NO_WARNINGS)
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /bigobj")
endif (WIN32)

set(BUILD_SHARED_LIBS ON)

include_directories(${PROJECT_SOURCE_DIR}/twsapi/Shared)

# Find includes in corresponding build directories
set(CMAKE_INCLUDE_CURRENT_DIR ON)

# Instruct CMake to run moc automatically when needed (for Qt).
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)

set(THREADS_PREFER_PTHREAD_FLAG ON)
FIND_PACKAGE(Threads)

# Find the Qt modules
find_package(Qt5Core REQUIRED)
find_package(Qt5Gui REQUIRED)
find_package(Qt5Widgets REQUIRED)
add_definitions(-DQT_DLL)
set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -DQT_NO_DEBUG")

set(Boost_USE_STATIC_LIBS OFF) 
set(Boost_USE_MULTITHREADED ON)  
set(Boost_USE_STATIC_RUNTIME OFF) 
add_definitions( -DBOOST_NETWORK_ENABLE_HTTPS )
add_definitions( -DBOOST_ALL_NO_LIB )
add_definitions( -DBOOST_LOG_DYN_LINK )
find_package(Boost 1.58.0 REQUIRED COMPONENTS thread timer date_time container log log_setup)
include_directories(${Boost_INCLUDE_DIRS})

file (GLOB IB_SAMPLE_SOURCE_FILES "${PROJECT_SOURCE_DIR}/*.h" "${PROJECT_SOURCE_DIR}/*.hpp" "${PROJECT_SOURCE_DIR}/*.cpp" "${PROJECT_SOURCE_DIR}/*.qrc")
file (GLOB IB_SAMPLE_SOURCE_FILES_TWSAPI "${PROJECT_SOURCE_DIR}/twsapi/Shared/*.h" "${PROJECT_SOURCE_DIR}/twsapi/Shared/*.cpp")
list(APPEND IB_SAMPLE_SOURCE_FILES ${IB_SAMPLE_SOURCE_FILES_TWSAPI})

add_executable(${PROJECT_NAME} MACOSX_BUNDLE ${IB_SAMPLE_SOURCE_FILES})

target_link_libraries(${PROJECT_NAME} ${Boost_LIBRARIES} Threads::Threads Qt5::Core Qt5::Widgets)
