ARCHS = arm64 arm64e
TARGET = iphone:clang:16.5:16.0
THEOS_PACKAGE_SCHEME = rootless
include $(THEOS)/makefiles/common.mk
LIBRARY_NAME = libCoreLyrics
libCoreLyrics_FILES = CoreLyrics.m ProviderModels.m DateLyricsProviderTiming.m vendor/qrc/DateLyricsQRCDes.m
libCoreLyrics_CFLAGS = -fobjc-arc -I$(THEOS_PROJECT_DIR) -Wall -Wextra -Wno-unused-function -Wno-unused-parameter
libCoreLyrics_FRAMEWORKS = Foundation QuartzCore
libCoreLyrics_LIBRARIES = z
libCoreLyrics_INSTALL_PATH = /usr/lib
include $(THEOS_MAKE_PATH)/library.mk

before-package::
	@find "$(THEOS_STAGING_DIR)" -type d -exec chmod 755 {} +
	@find "$(THEOS_STAGING_DIR)" -type f -exec chmod 644 {} +
	@chmod 755 "$(THEOS_STAGING_DIR)/usr/lib/libCoreLyrics.dylib" 2>/dev/null || chmod 755 "$(THEOS_STAGING_DIR)/var/jb/usr/lib/libCoreLyrics.dylib" 2>/dev/null || true
	@[ -f "$(THEOS_STAGING_DIR)/DEBIAN/postrm" ] && chmod 755 "$(THEOS_STAGING_DIR)/DEBIAN/postrm" || true
	@[ -f "$(THEOS_STAGING_DIR)/DEBIAN/postinst" ] && chmod 755 "$(THEOS_STAGING_DIR)/DEBIAN/postinst" || true
