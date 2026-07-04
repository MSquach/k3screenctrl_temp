include $(TOPDIR)/rules.mk

PKG_NAME:=luci-app-k3screenctrl
PKG_VERSION:=6
PKG_RELEASE:=1

PKG_LICENSE:=GPLv3
PKG_MAINTAINER:=Hill <lufanzhong@gmail.com>

PKG_BUILD_DIR:=$(BUILD_DIR)/$(PKG_NAME)

include $(INCLUDE_DIR)/package.mk

define Package/luci-app-k3screenctrl
  SECTION:=luci
  CATEGORY:=LuCI
  SUBMENU:=3. Applications
  DEPENDS:=+k3screenctrl +luci-base +luci-compat
  TITLE:=LuCI Support for k3screenctrl
  PKGARCH:=all
endef

define Package/luci-app-k3screenctrl/description
  LuCI web interface for k3screenctrl.
endef

define Build/Prepare
	mkdir -p $(PKG_BUILD_DIR)
	$(CP) ./luasrc $(PKG_BUILD_DIR)/
	$(CP) ./root $(PKG_BUILD_DIR)/
	$(CP) ./po $(PKG_BUILD_DIR)/
endef

PO2LMO:=$(STAGING_DIR_HOSTPKG)/bin/po2lmo

define Build/Compile
	mkdir -p $(PKG_BUILD_DIR)/i18n
	for po in `find $(PKG_BUILD_DIR)/po -type f -name '*.po'`; do \
		dir=`dirname $$$$po`; \
		lang=`basename $$$$dir`; \
		name=`basename $$$$po .po`; \
		$(PO2LMO) $$$$po $(PKG_BUILD_DIR)/i18n/$$$$name.$$$$lang.lmo; \
	done
endef

define Package/luci-app-k3screenctrl/install
	$(INSTALL_DIR) $(1)/usr/lib/lua/luci
	$(CP) $(PKG_BUILD_DIR)/luasrc/* $(1)/usr/lib/lua/luci/
	$(CP) $(PKG_BUILD_DIR)/root/* $(1)/
	$(INSTALL_DIR) $(1)/usr/lib/lua/luci/i18n
	$(INSTALL_DATA) $(PKG_BUILD_DIR)/i18n/*.lmo $(1)/usr/lib/lua/luci/i18n/
endef

define Package/luci-app-k3screenctrl/postinst
#!/bin/sh
[ -n "$${IPKG_INSTROOT}" ] || {
	rm -f /tmp/luci-indexcache.*
	rm -rf /tmp/luci-modulecache/
	killall -HUP rpcd 2>/dev/null
	exit 0
}
endef

$(eval $(call BuildPackage,$(PKG_NAME)))
