// Copyright (c) 2016, Thomas Goyne <plorkyeran@aegisub.org>
//
// Permission to use, copy, modify, and distribute this software for any
// purpose with or without fee is hereby granted, provided that the above
// copyright notice and this permission notice appear in all copies.
//
// THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
// WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
// MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
// ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
// WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
// ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
// OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
//
// Aegisub Project http://www.aegisub.org/

#include "font_file_lister.h"

#include "compat.h"

#include <libaegisub/charset_conv_win.h>
#include <libaegisub/fs.h>
#include <libaegisub/io.h>
#include <libaegisub/log.h>

#include <ShlObj.h>
#include <shlwapi.h>
#include <boost/scope_exit.hpp>
#include <unicode/utf16.h>
#include <Usp10.h>
#include <dwrite.h>
#include <windowsx.h>
#include <string.h>
#include <pathcch.h>

GdiFontFileLister::GdiFontFileLister(FontCollectorStatusCallback &cb)
: dwrite_factory_sh(nullptr, [](IDWriteFactory* p) { p->Release(); })
, font_collection_sh(nullptr, [](IDWriteFontCollection* p) { p->Release(); })
, dc_sh(nullptr, [](HDC dc) { DeleteDC(dc); })
, gdi_interop_sh(nullptr, [](IDWriteGdiInterop* p) { p->Release(); })
, test(cb)
{
	HRESULT hr;
	test(_("Creation GdiFontFileLister\n"), 0);

	IDWriteFactory* dwrite_factory;
	hr = DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(&dwrite_factory));
	if (FAILED(hr))
	{
		test(_("Fails DWriteCreateFactory\n"), 0);
		throw agi::EnvironmentError("Failed to initialize the DirectWrite Factory");
	}
	dwrite_factory_sh = dwrite_factory;

	IDWriteFontCollection* font_collection;
	hr = dwrite_factory_sh->GetSystemFontCollection(&font_collection, true);
	if (FAILED(hr))
	{
		test(_("Fails IDWriteFontCollection\n"), 0);
		throw agi::EnvironmentError("Failed to initialize the system font collection");
	}
	font_collection_sh = font_collection;

	HDC dc = CreateCompatibleDC(nullptr);
	if (dc == NULL) {
		test(_("Fails CreateCompatibleDC\n"), 0);
		throw agi::EnvironmentError("Failed to initialize the HDC");
	}
	dc_sh = dc;

	IDWriteGdiInterop* gdi_interop;
	hr = dwrite_factory_sh->GetGdiInterop(&gdi_interop);
	if (FAILED(hr))
	{
		test(_("Fails IDWriteGdiInterop\n"), 0);
		throw agi::EnvironmentError("Failed to initialize the Gdi Interop");
	}
	gdi_interop_sh = gdi_interop;
}


CollectionResult GdiFontFileLister::GetFontPaths(std::string const& facename, int bold, bool italic, std::vector<int> const& characters) {
	CollectionResult ret;

	int weight = bold == 0 ? 400 :
				 bold == 1 ? 700 :
				 bold;

	// From https://sourceforge.net/p/guliverkli2/code/HEAD/tree/src/subtitles/RTS.cpp#l45
	// AND https://sourceforge.net/p/guliverkli2/code/HEAD/tree/src/subtitles/STS.cpp#l2992
	LOGFONTW lf{};
	lf.lfCharSet = DEFAULT_CHARSET; // in the best world, we should use the one specified in the ass file
	wcsncpy(lf.lfFaceName, agi::charset::ConvertW(facename).c_str(), LF_FACESIZE);
	lf.lfItalic = italic ? -1 : 0;
	lf.lfWeight = weight;
	lf.lfOutPrecision = OUT_TT_PRECIS;
	lf.lfClipPrecision = CLIP_DEFAULT_PRECIS;
	lf.lfQuality = ANTIALIASED_QUALITY;
	lf.lfPitchAndFamily = DEFAULT_PITCH | FF_DONTCARE;


	// Gather all of the styles for the given family name
	std::vector<LOGFONTW> matches;
	using type = decltype(matches);
	EnumFontFamiliesEx(dc_sh, &lf, [](const LOGFONT *lf, const TEXTMETRIC *, DWORD, LPARAM lParam) -> int {
		reinterpret_cast<type*>(lParam)->push_back(*lf);
		return 1;
	}, (LPARAM)&matches, 0);

	if (matches.empty())
		return ret;

	// If the user asked for a non-regular style, verify that it actually exists
	if (italic || bold) {
		bool has_bold = false;
		bool has_italic = false;
		bool has_bold_italic = false;

		auto is_italic = [&](LOGFONTW const& lf) {
			return !italic || lf.lfItalic;
		};
		auto is_bold = [&](LOGFONTW const& lf) {
			return !bold
				|| (bold == 1 && lf.lfWeight >= 700)
				|| (bold > 1 && lf.lfWeight > bold);
		};

		for (auto const& match : matches) {
			has_bold = has_bold || is_bold(match);
			has_italic = has_italic || is_italic(match);
			has_bold_italic = has_bold_italic || (is_bold(match) && is_italic(match));
		}

		ret.fake_italic = !has_italic;
		ret.fake_bold = (italic && has_italic ? !has_bold_italic : !has_bold);
	}


	HFONT hfont = CreateFontIndirect(&lf);
	if (hfont == NULL) {
		test(_("Fails CreateFontIndirect\n"), 0);
		return ret;
	}
	BOOST_SCOPE_EXIT_ALL(=) {
		SelectObject(dc_sh, nullptr);
		DeleteObject(hfont);
	};

	SelectFont(dc_sh, hfont);

	HRESULT hr;
	IDWriteFontFace* font_face;
	hr = gdi_interop_sh->CreateFontFaceFromHdc(dc_sh, &font_face);
	if (FAILED(hr))
	{
		test(_("Fails CreateFontFaceFromHdc\n"), 0);
		return ret;
	}
	agi::scoped_holder<IDWriteFontFace*> font_face_sh(font_face, [](IDWriteFontFace* p) { p->Release(); });

	
	IDWriteFont* font;
	hr = font_collection_sh->GetFontFromFontFace(font_face_sh, &font);
	if (FAILED(hr))
	{
		test(_("Fails GetFontFromFontFace\n"), 0);
		return ret;
	}
	agi::scoped_holder<IDWriteFont*> font_sh(font, [](IDWriteFont* p) { p->Release(); });


    UINT32 file_count = 1;
	IDWriteFontFile* font_file;
    // DirectWrite only supports one file per face
	hr = font_face_sh->GetFiles(&file_count, &font_file);
	if (FAILED(hr))
	{
		test(_("Fails GetFiles\n"), 0);
		return ret;
	}
	agi::scoped_holder<IDWriteFontFile*> font_file_sh(font_file, [](IDWriteFontFile* p) { p->Release(); });


	LPCVOID font_file_reference_key;
	UINT32 font_file_reference_key_size;
	hr = font_file_sh->GetReferenceKey(&font_file_reference_key, &font_file_reference_key_size);
	if (FAILED(hr))
	{
		test(_("Fails GetReferenceKey\n"), 0);
		return ret;
	}


	IDWriteFontFileLoader* loader;
	hr = font_file_sh->GetLoader(&loader);
	if (FAILED(hr))
	{
		test(_("Fails GetLoader\n"), 0);
		return ret;
	}
	agi::scoped_holder<IDWriteFontFileLoader*> loader_sh(loader, [](IDWriteFontFileLoader* p) { p->Release(); });


	IDWriteLocalFontFileLoader* local_loader;
	hr = loader->QueryInterface(__uuidof(IDWriteLocalFontFileLoader), (void**)&local_loader);
	if (FAILED(hr))
	{
		test(_("Fails QueryInterface\n"), 0);
		return ret;
	}
	agi::scoped_holder<IDWriteLocalFontFileLoader*> local_loader_sh(local_loader, [](IDWriteLocalFontFileLoader* p) { p->Release(); });


	UINT32 path_length;
	hr = local_loader_sh->GetFilePathLengthFromKey(font_file_reference_key, font_file_reference_key_size, &path_length);
	if (FAILED(hr))
	{
		test(_("Fails GetFilePathLengthFromKey\n"), 0);
		return ret;
	}


	WCHAR* path = new WCHAR[path_length + 1];
	hr = local_loader_sh->GetFilePathFromKey(font_file_reference_key, font_file_reference_key_size, path, path_length + 1);
	if (FAILED(hr))
	{
		test(_("Fails GetFilePathFromKey\n"), 0);
		return ret;
	}

	test(_("Test\n"), 0);

	HANDLE hfile = CreateFile(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
	if(hfile == INVALID_HANDLE_VALUE) {
		test(_("Fails CreateFile\n"), 0);
		return ret;
	}
	agi::scoped_holder<HANDLE> hfile_sh(hfile, [](HANDLE hfile) { CloseHandle(hfile);; });

	DWORD normalized_path_length = GetFinalPathNameByHandle(hfile_sh, nullptr, 0, FILE_NAME_NORMALIZED | VOLUME_NAME_NONE);
	if (!normalized_path_length) {
		test(_("Fails GetFinalPathNameByHandle 1\n"), 0);
		return ret;
	}

	WCHAR* normalized_path = new WCHAR[normalized_path_length + 1];
	normalized_path_length = GetFinalPathNameByHandle(hfile_sh, normalized_path, normalized_path_length + 1, FILE_NAME_NORMALIZED | VOLUME_NAME_NONE);
	if (!normalized_path_length) {
		test(_("Fails GetFinalPathNameByHandle 2\n"), 0);
		return ret;
	}
	//wmemmove(normalized_path, normalized_path + 4, normalized_path_length - 3); // skip the \\?\ added by GetFinalPathNameByHandle

	DWORD normalized_no_path_length = GetFullPathName(normalized_path, normalized_path_length, nullptr, nullptr);
	if (!normalized_no_path_length) {
		test(_("Fails GetFullPathName 1\n"), 0);
		return ret;
	}

	WCHAR* normalized_no_path = new WCHAR[normalized_no_path_length + 1];
	normalized_path_length = GetFullPathName(normalized_path, normalized_path_length, normalized_no_path, nullptr);
	if (!normalized_path_length) {
		test(_("Fails GetFullPathName 2\n"), 0);
		return ret;
	}

	// TODO normalize case
	ret.paths.push_back(agi::fs::path(normalized_no_path));

	BOOL exists;
	for (int character : characters) {
		hr = font_sh->HasCharacter((UINT32)character, &exists);
		if (FAILED(hr) || !exists) {
			ret.missing += character;
		}
	}


	return ret;
}
