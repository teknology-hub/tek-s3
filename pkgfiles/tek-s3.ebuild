# Copyright 2025 Gentoo Authors
# Distributed under the terms of the GNU General Public License v2

EAPI=8

inherit meson

DESCRIPTION="A server for sharing access to downloading Steam app files"
HOMEPAGE="https://github.com/teknology-hub/tek-s3"
SRC_URI="
https://github.com/teknology-hub/tek-s3/releases/download/v${PV}/${P}.tar.xz
https://github.com/TinyTinni/ValveFileVDF/archive/refs/tags/v1.1.1.tar.gz
"

LICENSE="GPL-3+"
SLOT="0"
KEYWORDS="amd64"

IUSE="+brotli systemd zlib-ng +zstd"

COMMON_DEPEND="
	games-util/tek-steamclient:2
	net-libs/libwebsockets[client,extensions,ssl]
	brotli? ( app-arch/brotli )
	systemd? ( sys-apps/systemd )
	zlib-ng? ( sys-libs/zlib-ng )
	!zlib-ng? ( virtual/zlib )
	zstd? ( app-arch/zstd )
"
DEPEND="
	${COMMON_DEPEND}
	dev-libs/rapidjson
"
RDEPEND="${COMMON_DEPEND}"

src_prepare() {
	default

	mv "${WORKDIR}/ValveFileVDF-1.1.1" "${S}/subprojects/" || die
	cp "${S}/subprojects/packagefiles/ValveFileVDF/meson.build" "${S}/subprojects/ValveFileVDF-1.1.1/" || die
}

src_configure() {
	local emesonargs=(
		$(meson_feature brotli)
		$(meson_feature systemd)
		$(meson_feature zlib-ng zlib_ng)
		$(meson_feature zstd)
	)
	meson_src_configure
}
