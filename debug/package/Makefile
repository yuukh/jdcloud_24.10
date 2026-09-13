include $(TOPDIR)/rules.mk

PKG_NAME:=hnat418-debug
PKG_VERSION:=1
PKG_RELEASE:=1
PKG_LICENSE:=GPL-2.0-only

include $(INCLUDE_DIR)/package.mk

define Package/hnat418-debug
  SECTION:=utils
  CATEGORY:=Utilities
  TITLE:=Bounded HNAT diagnostic sessions
  DEPENDS:=@TARGET_mediatek +iperf3 +ip-full +ss +tc-tiny +ethtool +nftables-json +openssh-sftp-server +coreutils-timeout
endef

define Build/Compile
endef

define Package/hnat418-debug/install
	$(INSTALL_DIR) $(1)/usr/sbin $(1)/usr/share/hnat418
	$(INSTALL_BIN) ./files/hnat418-run $(1)/usr/sbin/
	$(INSTALL_BIN) ./files/hnat418-snapshot $(1)/usr/sbin/
	$(INSTALL_DATA) ./files/build-manifest.txt $(1)/usr/share/hnat418/
endef

$(eval $(call BuildPackage,hnat418-debug))
