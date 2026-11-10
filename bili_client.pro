TEMPLATE = app
TARGET = BiliClient
CONFIG += qt warn_on cascades10
QT += network

# libcurl：BB10 的 QtNetwork HTTPS 握手挂起，改用 curl+OpenSSL 栈（与 BBTube 的 libbbnet 同源）
# 注意：不可链接 -lslog2！描述符 LD_LIBRARY_PATH 不含 /usr/lib，运行时找不到 libslog2.so 会闪退。
# 日志走文件（tmp/bili_log.txt）+ 屏幕诊断即可。
LIBS += -lbbdata -lbbsystem -lcurl -lsqlite3

SOURCES += \
    src/main.cpp \
    src/applicationui.cpp \
    src/bili_api.cpp \
    src/feed_model.cpp \
    src/qrcodegen.cpp \
    src/video_store.cpp \
    src/note_model.cpp \
    src/note_store.cpp \
    src/collect_store.cpp \
    src/history_store.cpp \
    src/follow_store.cpp \
    src/local_db.cpp

HEADERS += \
    src/applicationui.hpp \
    src/bili_api.hpp \
    src/feed_model.hpp \
    src/qrcodegen.hpp \
    src/video_store.hpp \
    src/note_model.hpp \
    src/note_store.hpp \
    src/collect_store.hpp \
    src/history_store.hpp \
    src/follow_store.hpp \
    src/local_db.hpp

device {
    CONFIG(release, debug|release) {
        DEFINES += QT_NO_DEBUG_OUTPUT
    }
}

# 不设置 OBJECTS_DIR/MOC_DIR：bbndk 10.1 的 qmake 在跨目录构建时会把
# 相对路径错算成 /.obj 与 ../../.obj 两种不一致写法，导致 moc 规则失效。
# 使用默认布局（.o 与 moc_*.cpp 直接落在构建目录内）即可稳定构建。

# 将 assets 目录复制到产物目录（随 .so 一起打包）
assets.files = assets
assets.path = .
DEPLOYMENT += assets
