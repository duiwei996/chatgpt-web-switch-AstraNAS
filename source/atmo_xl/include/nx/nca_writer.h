/*
Copyright (c) 2017-2018 Adubbz

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
*/

#pragma once
#include <switch.h>
#include <array>
#include <vector>
#include "nx/ncm.hpp"
#include <memory>
#include "install/nca.hpp"
#include "sha256.hpp"

class NcaBodyWriter
{
public:
	NcaBodyWriter(const NcmContentId& ncaId, u64 offset, u64 expectedSize,
	              std::shared_ptr<nx::ncm::ContentStorage>& contentStorage,
	              AstraSha256Context* hashContext,
	              const NcmPlaceHolderId& placeholderId);
	virtual ~NcaBodyWriter();
	virtual u64 write(const  u8* ptr, u64 sz);
	virtual bool close();
	
	bool isOpen() const;

protected:
	std::shared_ptr<nx::ncm::ContentStorage> m_contentStorage;
	NcmContentId m_ncaId;
	NcmPlaceHolderId m_placeholderId;

	u64 m_offset;
	u64 m_expectedSize;
	AstraSha256Context* m_hashContext = nullptr;
	bool m_closed = false;
};

class NcaWriter
{
public:
	NcaWriter(const NcmContentId& ncaId,
	          std::shared_ptr<nx::ncm::ContentStorage>& contentStorage,
	          const u8* expectedHash = nullptr);
	NcaWriter(const NcmContentId& ncaId, const NcmPlaceHolderId& placeholderId,
	          std::shared_ptr<nx::ncm::ContentStorage>& contentStorage,
	          const u8* expectedHash = nullptr);
	virtual ~NcaWriter();

	bool isOpen() const;
	bool close();
	u64 write(const  u8* ptr, u64 sz);
	void flushHeader();

protected:
	NcmContentId m_ncaId;
	NcmPlaceHolderId m_placeholderId;
	std::shared_ptr<nx::ncm::ContentStorage> m_contentStorage;
	std::vector<u8> m_buffer;
	std::vector<u8> m_bodyProbe;
	std::shared_ptr<NcaBodyWriter> m_writer;
	AstraSha256Context m_hashContext;
	std::array<u8, 32> m_expectedHash{};
	std::array<u8, 32> m_actualHash{};
	u64 m_ncaSize = 0;
	bool m_hasExpectedHash = false;
	bool m_hashFinalized = false;
	bool m_headerFlushed = false;
	bool m_closed = false;
};
