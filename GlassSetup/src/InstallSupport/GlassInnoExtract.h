#pragma once
// Strict, non-executing Inno 6.7 payload reader. Authenticode verification of
// the setup and EACH extracted PE is the caller's independent responsibility.
#include "../../third_party/lzma/LzmaDec.h"
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <stdexcept>
#include <vector>

namespace GlassInnoExtract {
using Bytes=std::vector<unsigned char>;
constexpr std::size_t MaxInput=128u*1024*1024,MaxOutput=128u*1024*1024,MaxHeader=16u*1024*1024;
struct Span {
    const unsigned char* data=nullptr;std::size_t size=0;
    Span Sub(std::uint64_t offset,std::uint64_t length) const {
        if(offset>size||length>size-offset)throw std::runtime_error("Inno range outside input");
        return {data+static_cast<std::size_t>(offset),static_cast<std::size_t>(length)};
    }
    std::uint64_t LE(std::size_t at,unsigned count) const {
        const auto s=Sub(at,count);std::uint64_t result=0;
        for(unsigned i=0;i<count;++i)result|=std::uint64_t(s.data[i])<<(8*i);
        return result;
    }
};
inline Span View(const Bytes& bytes){return {bytes.data(),bytes.size()};}
inline std::uint32_t Crc(Span s) {
    std::uint32_t crc=~0u;
    for(std::size_t i=0;i<s.size;++i){crc^=s.data[i];for(unsigned j=0;j<8;++j)crc=(crc>>1)^(0xEDB88320u&(~(crc&1u)+1u));}
    return ~crc;
}
inline void CheckCancel(const std::function<bool()>& cancelled) {
    if(cancelled&&cancelled())throw std::runtime_error("Inno extraction cancelled");
}
inline Bytes Decode(Span in,std::size_t limit,const std::function<bool()>& cancelled) {
    if(in.size<5||in.LE(1,4)>64u*1024*1024)throw std::runtime_error("Invalid or excessive LZMA dictionary");
    ISzAlloc allocator{[](ISzAllocPtr,std::size_t n)->void*{return std::malloc(n);},[](ISzAllocPtr,void* p){std::free(p);}};
    CLzmaDec decoder;LzmaDec_Construct(&decoder);
    if(LzmaDec_Allocate(&decoder,in.data,5,&allocator)!=SZ_OK)throw std::runtime_error("LZMA allocation/properties rejected");
    struct Release {CLzmaDec& d;ISzAlloc& a;~Release(){LzmaDec_Free(&d,&a);}} release{decoder,allocator};
    LzmaDec_Init(&decoder);std::size_t pos=5;Bytes out;std::array<unsigned char,65536> buffer{};
    for(;;) {
        CheckCancel(cancelled);SizeT consumed=in.size-pos,written=buffer.size();ELzmaStatus status{};
        const auto result=LzmaDec_DecodeToBuf(&decoder,buffer.data(),&written,in.data+pos,&consumed,LZMA_FINISH_ANY,&status);
        if(result!=SZ_OK||written>limit-out.size())throw std::runtime_error("LZMA corrupt or output limit exceeded");
        pos+=consumed;out.insert(out.end(),buffer.begin(),buffer.begin()+written);
        if(status==LZMA_STATUS_FINISHED_WITH_MARK) {
            if(pos!=in.size)throw std::runtime_error("Trailing LZMA data");
            return out;
        }
        if(!written&&!consumed)throw std::runtime_error("LZMA missing EOS");
    }
}
inline Bytes HeaderBlock(Span input,std::size_t& pos,const std::function<bool()>& cancelled) {
    const auto fixed=input.Sub(pos,13);const auto size=fixed.LE(4,8);const auto compressed=fixed.LE(12,1);
    if(Crc(fixed.Sub(4,9))!=fixed.LE(0,4)||size>MaxHeader||compressed>1)throw std::runtime_error("Inno header CRC/size/compression");
    pos+=13;const auto stored=input.Sub(pos,size);pos+=static_cast<std::size_t>(size);
    Bytes merged;std::size_t at=0;
    while(at<stored.size) {
        CheckCancel(cancelled);
        const auto expected=stored.LE(at,4);at+=4;
        const auto n=std::min<std::size_t>(4096,stored.size-at);const auto part=stored.Sub(at,n);
        if(!n||Crc(part)!=expected)throw std::runtime_error("Inno header chunk CRC");
        merged.insert(merged.end(),part.data,part.data+part.size);at+=n;
    }
    return compressed?Decode(View(merged),MaxHeader,cancelled):merged;
}
// Decode separately per FILE and per 64K block. Never carry a partial opcode
// across a boundary; AddrOffset uses modulo-2^32 arithmetic, as Inno does.
inline void UndoCalls(Bytes& file) {
    for(std::size_t base=0;base<file.size();base+=65536) {
        const auto n=std::min<std::size_t>(65536,file.size()-base);if(n<5)continue;
        for(std::size_t i=0;i<n-4;) {
            auto* p=file.data()+base;
            if(p[i]!=0xE8&&p[i]!=0xE9){++i;continue;}
            ++i;
            if(p[i+3]==0||p[i+3]==255) {
                const auto addr=static_cast<std::uint32_t>(base+i+4)&0xFFFFFFu;
                const std::uint32_t rel=(std::uint32_t(p[i])|(std::uint32_t(p[i+1])<<8)|(std::uint32_t(p[i+2])<<16))-addr;
                if(rel&0x800000u)p[i+3]=static_cast<unsigned char>(~p[i+3]);
                p[i]=static_cast<unsigned char>(rel);p[i+1]=static_cast<unsigned char>(rel>>8);p[i+2]=static_cast<unsigned char>(rel>>16);
            }
            i+=4;
        }
    }
}
struct File {Bytes bytes;std::array<unsigned char,32> sha256{};std::uint32_t versionMS=0,versionLS=0;};
// offsets is the EXACT 64-byte RT_RCDATA/11111 resource, never a signature scan.
// No paths from the archive are used, and no files are written by this parser.
inline std::vector<File> Extract(Span input,Span offsets,const std::function<bool()>& cancelled={}) {
    const unsigned char signature[]={ 'r','D','l','P','t','S',0xCD,0xE6,0xD7,0x7B,0x0B,0x2A };
    if(input.size>MaxInput||offsets.size!=64||std::memcmp(offsets.data,signature,12)||offsets.LE(12,4)!=2||
       offsets.LE(16,8)>input.size||Crc(offsets.Sub(0,60))!=offsets.LE(60,4))throw std::runtime_error("Inno offset table rejected");
    auto h=offsets.LE(40,8),body=offsets.LE(48,8);
    const auto isHeader=[&](std::uint64_t at){return at<=input.size&&input.size-at>=64&&std::memcmp(input.data+at,"Inno Setup",10)==0;};
    if(!isHeader(h)&&isHeader(body))std::swap(h,body);
    const char expected[]="Inno Setup Setup Data (6.7.0)";
    const auto title=input.Sub(h,64);
    if(std::memcmp(title.data,expected,sizeof(expected))||
       !std::all_of(title.data+sizeof(expected),title.data+64,[](unsigned char c){return c==0;}))
        throw std::runtime_error("Only unmodified Inno 6.7.0 format is supported");
    std::size_t pos=static_cast<std::size_t>(h)+64+53;input.Sub(h,64+53);
    (void)HeaderBlock(input,pos,cancelled);const auto records=HeaderBlock(input,pos,cancelled);
    if(records.empty()||records.size()%89||records.size()/89>256)throw std::runtime_error("Invalid Inno records");
    struct Chunk {std::uint64_t size;bool compressed;Bytes bytes;};
    std::map<std::uint64_t,Chunk> chunks;std::vector<File> files;std::size_t total=0;
    for(std::size_t r=0;r<records.size();r+=89) {
        CheckCancel(cancelled);const auto rec=View(records).Sub(r,89);
        if(rec.LE(0,4)>1||rec.LE(0,4)!=rec.LE(4,4))throw std::runtime_error("External Inno slices not supported");
        const auto offset=rec.LE(8,8),fileAt=rec.LE(16,8),size=rec.LE(24,8),chunkSize=rec.LE(32,8);
        const auto flags=rec.LE(88,1);const bool compressed=(flags&0x10)!=0;
        if(size>MaxOutput-total||offset>input.size||body>input.size-offset)throw std::runtime_error("Inno file/output bounds");
        if(!chunks.count(offset)) {
            const auto chunk=input.Sub(body+offset,chunkSize);
            if(chunk.size<4||std::memcmp(chunk.data,"zlb\x1A",4))throw std::runtime_error("Inno chunk signature");
            const auto stream=chunk.Sub(4,chunk.size-4);
            Bytes bytes=compressed?Decode(stream,MaxOutput,cancelled):Bytes(stream.data,stream.data+stream.size);
            std::size_t cached=bytes.size();for(const auto& entry:chunks)cached+=entry.second.bytes.size();
            if(cached>MaxOutput)throw std::runtime_error("Inno chunk cache limit");
            chunks.emplace(offset,Chunk{chunkSize,compressed,std::move(bytes)});
        }
        const auto& chunk=chunks.at(offset);
        if(chunk.size!=chunkSize||chunk.compressed!=compressed)throw std::runtime_error("Conflicting Inno chunk descriptors");
        const auto data=View(chunk.bytes).Sub(fileAt,size);File file;
        file.bytes.assign(data.data,data.data+data.size);if(flags&4)UndoCalls(file.bytes);
        std::copy(rec.data+40,rec.data+72,file.sha256.begin());
        file.versionMS=static_cast<std::uint32_t>(rec.LE(80,4));file.versionLS=static_cast<std::uint32_t>(rec.LE(84,4));
        total+=file.bytes.size();files.push_back(std::move(file));
    }
    return files;
}
}
