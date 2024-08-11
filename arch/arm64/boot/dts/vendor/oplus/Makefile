ifeq ($(CONFIG_BUILD_ARM64_DT_OVERLAY),y)
dtbo-$(CONFIG_ARCH_WAIPIO) +=  \
	wly-20846-waipio-overlay.dtbo \
	wly-2084A-waipio-overlay.dtbo \
	wly-20846-waipio-overlay-EVB.dtbo \
	wly-2084A-waipio-overlay-EVB.dtbo \
	wly-20846-waipio-overlay-preT0.dtbo \
	wly-2084A-waipio-overlay-preT0.dtbo \

endif

always-y	:= $(dtb-y)
subdir-y	:= $(dts-dirs)
clean-files	:= *.dtb *.dtbo
