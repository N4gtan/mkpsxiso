#ifndef _CDWRITER_H
#define _CDWRITER_H

#include "cd.h"
#include "mmappedfile.h"
#include <ThreadPool.h>
#include <forward_list>

namespace cd {
using namespace progschj;

class IsoWriter
{
public:
	enum class EdcEccForm
	{
		None = 0,
		Form1,
		Form2,
		Autodetect,
	};
		
	enum {
		SubData	= 0x00080000, // Data
		SubSTR	= 0x00480100, // Stream (0x01: CN, 0x48: RT | Data)
		SubEOR	= 0x00090000, // End of Record
		SubEOX	= 0x00890000, // End of Extent (0x89: EOF | Data | EOR)
	};

	class SectorView
	{
	public:
		SectorView(ThreadPool* threadPool, MMappedFile* mappedFile, unsigned int offsetLBA, unsigned int sizeLBA, EdcEccForm edcEccForm);
		virtual ~SectorView();

		virtual void WriteFile(FILE* file) = 0;
		virtual void WriteMemory(const void* memory, size_t size, const bool setEOX = true) = 0;
		virtual void WriteBlankSectors(unsigned int count, const unsigned char submode = 0x20, const bool eccAddr = false) = 0;
		virtual size_t GetSpaceInCurrentSector() const = 0;
		virtual void NextSector() = 0;
		virtual void SetSubheader(unsigned int subHead) = 0;

		void WaitForChecksumJobs();

	protected:
		void PrepareSectorHeader() const;

		void CalculateForm1(const bool eccAddr = false);
		void CalculateForm2();

		void* m_currentSector = nullptr;
		size_t m_offsetInSector = 0;
		unsigned int m_currentLBA = 0;

		const unsigned int m_endLBA = 0;
		const EdcEccForm m_edcEccForm = EdcEccForm::None;

	private:
		std::forward_list<std::future<void>> m_checksumJobs;
		ThreadPool* m_threadPool;
		MMappedFile::View m_view;
	};

	class RawSectorView
	{
	public:
		RawSectorView(MMappedFile* mappedFile, unsigned int offsetLBA, unsigned int sizeLBA);

		void* GetRawBuffer() const;
		void WriteBlankSectors();

	private:
		MMappedFile::View m_view;
		unsigned int m_endLBA;
	};

	IsoWriter() = default;

	bool Create(const fs::path& fileName, unsigned int sizeLBA);
	void Close();

	std::unique_ptr<SectorView> GetSectorViewM2F1(unsigned int offsetLBA, unsigned int sizeLBA, EdcEccForm edcEccForm) const;
	std::unique_ptr<SectorView> GetSectorViewM2F2(unsigned int offsetLBA, unsigned int sizeLBA, EdcEccForm edcEccForm) const;
	std::unique_ptr<RawSectorView> GetRawSectorView(unsigned int offsetLBA, unsigned int sizeLBA) const;

private:
	std::unique_ptr<MMappedFile> m_mmap;
	std::unique_ptr<ThreadPool> m_threadPool;
};

};

#endif // _CDWRITER_H
