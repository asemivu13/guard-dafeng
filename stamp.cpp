// inplace_incremental_stamp.cpp
// ----------------------------------------------------------------------------------
// In‑place streaming stamper with true incremental save.
// Copies the original PDF bytes into the output file, then appends only the
// deltas after each batch of pages is stamped. Peak RAM ≈ one page’s objects.
// ----------------------------------------------------------------------------------

#define NOMINMAX
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>
#include <set>
#include <algorithm>

// ─── Cross‑platform RSS probe ───────────────────────────────────────────────
#if defined(_WIN32)
  #include <windows.h>
  #include <psapi.h>
  static size_t CurrentRSS(){
    PROCESS_MEMORY_COUNTERS_EX p;
    return GetProcessMemoryInfo(GetCurrentProcess(),
      reinterpret_cast<PPROCESS_MEMORY_COUNTERS>(&p),
      sizeof(p)) ? p.WorkingSetSize : 0;
  }
#else
  #include <sys/resource.h>
  static size_t CurrentRSS(){
    struct rusage r{};
    return !getrusage(RUSAGE_SELF,&r) ? size_t(r.ru_maxrss)*1024 : 0;
  }
#endif
static void Mem(const char* tag){
  std::cerr << "[MEM] " << tag << ": " << CurrentRSS()/1048576.0 << " MB\n";
}

// ─── PDFium headers ─────────────────────────────────────────────────────────
#include "public/fpdf_edit.h"
#include "public/fpdf_save.h"
#include "public/fpdfview.h"

// ─── Streaming input ─────────────────────────────────────────────────────────
struct IReader {
  virtual ~IReader() = default;
  virtual bool Read(unsigned long, unsigned char*, unsigned long) = 0;
  virtual unsigned long long Len() const = 0;
};
struct FileReader final : IReader {
  std::ifstream f;
  unsigned long long n{};
  explicit FileReader(const std::string& p)
    : f(p, std::ios::binary)
  {
    if (!f) throw std::runtime_error("open " + p);
    f.seekg(0, std::ios::end);
    n = f.tellg();
  }
  bool Read(unsigned long off,
            unsigned char* buf,
            unsigned long sz) override
  {
    f.seekg(off);
    f.read(reinterpret_cast<char*>(buf), sz);
    return f.gcount() == (std::streamsize)sz;
  }
  unsigned long long Len() const override { return n; }
};
static FPDF_BOOL GetBlock(void* p,
                          unsigned long pos,
                          unsigned char* b,
                          unsigned long sz)
{
  return static_cast<IReader*>(p)->Read(pos, b, sz);
}

// ─── Streaming output ───────────────────────────────────────────────────────
struct FileWriter : FPDF_FILEWRITE {
  FILE* fp;
  static int W(FPDF_FILEWRITE* s,
               const void* buf,
               unsigned long sz)
  {
    return std::fwrite(buf, 1, sz,
                       static_cast<FileWriter*>(s)->fp) == sz;
  }
  explicit FileWriter(const char* path,
                      const char* mode = "wb")
  {
    version = 1;
    WriteBlock = &W;
    fp = std::fopen(path, mode);
    if (!fp) throw std::runtime_error("open out");
  }
  ~FileWriter(){
    if (fp) std::fclose(fp);
  }
};

// helper: utf‑16le quick
static std::u16string u16(const std::string& s){
  std::u16string o;
  o.reserve(s.size());
  for (unsigned char c : s) o.push_back(c);
  return o;
}

// ─── pageSpec parser (supports "all", "1,3,5", "10-20") ──────────────────
static std::set<int> parseSpec(const std::string& spec,
                               int maxPage)
{
  std::set<int> idx;
  if (spec == "all") {
    for (int i = 1; i <= maxPage; ++i)
      idx.insert(i);
    return idx;
  }
  std::stringstream ss(spec);
  std::string tok;
  while (std::getline(ss, tok, ',')) {
    size_t dash = tok.find('-');
    if (dash == std::string::npos) {
      int p = std::stoi(tok);
      if (p >= 1 && p <= maxPage) idx.insert(p);
    } else {
      int a = std::stoi(tok.substr(0, dash)),
          b = std::stoi(tok.substr(dash+1));
      if (a > b) std::swap(a,b);
      a = std::max(a,1);
      b = std::min(b,maxPage);
      for (int p = a; p <= b; ++p) idx.insert(p);
    }
  }
  return idx;
}

// ─── Core stamping routine ──────────────────────────────────────────────────
bool InplaceIncrementalStamp(IReader& rdr,
                             const std::string& spec,
                             const std::string& text,
                             const std::string& inPath,
                             const std::string& outPath)
{
  Mem("start");
  // 1) Init PDFium
  FPDF_LIBRARY_CONFIG cfg{2,nullptr,0,0,0};
  FPDF_InitLibraryWithConfig(&cfg);

  // 2) Open original document
  FPDF_FILEACCESS fa{};
  fa.m_FileLen = rdr.Len();
  fa.m_Param   = &rdr;
  fa.m_GetBlock = &GetBlock;
  FPDF_DOCUMENT doc = FPDF_LoadCustomDocument(&fa,nullptr);
  if (!doc) {
    std::cerr << "Load err " << FPDF_GetLastError() << "\n";
    return false;
  }
  Mem("after load");

  int pageCount = FPDF_GetPageCount(doc);
  auto wanted = parseSpec(spec, pageCount);
  if (wanted.empty()) {
    std::cerr << "no pages matched\n";
    return false;
  }
  auto wtxt = u16(text);

  // 3) Prepare output for true incremental
  FileWriter fw(outPath.c_str(), "wb+");
  {
    // 3a) Copy original bytes into out file
    std::ifstream src(inPath, std::ios::binary);
    if (!src) throw std::runtime_error("open input for copy");
    std::vector<char> buf(1<<20);
    while (src) {
      src.read(buf.data(), buf.size());
      std::fwrite(buf.data(), 1, src.gcount(), fw.fp);
    }
    // 3b) Rewind so first incremental save can append
    std::fflush(fw.fp);
    std::rewind(fw.fp);
  }

  // 4) Batch‐stamp & incremental save
  const int STEP = 40;
  for (int start = 1; start <= pageCount; start += STEP) {
    int end = std::min(start + STEP - 1, pageCount);

    for (int p = start; p <= end; ++p) {
      if (wanted.find(p) == wanted.end()) continue;
      FPDF_PAGE page = FPDF_LoadPage(doc, p-1);
      if (!page) continue;

      // create & style text
      FPDF_PAGEOBJECT t = FPDFPageObj_NewTextObj(doc, "Helvetica", 24.f);
      FPDFPageObj_SetFillColor(t, 0, 0, 255, 255); // bright blue
      FPDFText_SetText(t, (const FPDF_WCHAR*)wtxt.c_str());

      float l, b, r, tb;
      FPDFPageObj_GetBounds(t, &l,&b,&r,&tb);
      double pw = FPDF_GetPageWidthF(page);

      // center bottom
      FPDFPageObj_Transform(t, 1,0,0,1,
                            (pw - (r-l))/2.0,
                            30.0 - b);

      FPDFPage_InsertObject(page, t);
      FPDFPage_GenerateContent(page);
      FPDF_ClosePage(page);
    }

    // incremental append of only the deltas
    if (!FPDF_SaveWithVersion(doc, &fw, FPDF_INCREMENTAL, 17)) {
      std::cerr << "save err\n";
      return false;
    }

    Mem(("after batch " + std::to_string(end)).c_str());
  }

  // 5) Cleanup
  FPDF_CloseDocument(doc);
  FPDF_DestroyLibrary();
  Mem("end");
  return true;
}

// ─── CLI ────────────────────────────────────────────────────────────────────
int main(int ac, char** av){
  if (ac < 2) {
    std::cerr << "usage: inplace_incremental_stamp <pdf> [pageSpec] [out.pdf] [text]\n";
    return 1;
  }
  std::string in  = av[1];
  std::string spec= ac>=3?av[2]:"all";
  std::string out = ac>=4?av[3]:"stamped.pdf";
  std::string txt = ac>=5?av[4]:"Stamped by Asem";

  try {
    FileReader rdr(in);
    if (!InplaceIncrementalStamp(rdr, spec, txt, in, out))
      return 1;
    std::cout << "✔ wrote " << out << "\n";
  } catch (const std::exception& e) {
    std::cerr << e.what() << "\n";
    return 1;
  }
  return 0;
}
