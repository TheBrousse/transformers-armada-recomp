#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <utility>
#include <vector>

namespace nr::microcode_prefix {
constexpr size_t ram_size=0x02000000u, max_words=65536u, max_receipts=8u;
struct Prefix { uint64_t epoch=0; std::vector<uint32_t> words; };
struct Receipt {
 uint64_t epoch=0; int thread=0;
 uint32_t sp=0,ra=0,pool=0,slot=0,source=0,qwords=0,before=0;
};
inline bool cpuRamAddress(uint32_t a) {
 const uint32_t view=a&0xe0000000u;
 return view==0u||view==0x20000000u||view==0x80000000u||view==0xa0000000u;
}
inline bool span(uint32_t a,size_t n) {
 const uint32_t p=a&0x1fffffffu;
 return n&&n<=ram_size&&cpuRamAddress(a)&&a<=UINT32_MAX-(n-1u)&&p<=ram_size-n;
}
inline bool word(const uint8_t*r,size_t bytes,uint32_t a,uint32_t&v) {
 const uint32_t p=a&0x1fffffffu;
 if(!r||bytes<ram_size||!span(a,4))return false;
 std::memcpy(&v,r+p,4);return true;
}
// Exact native 225300 DMA protocol: REF(data), FLUSH, MPG. Parse the
// complete transaction before publishing any command; no live VU memory read.
inline bool parse(const uint8_t*r,size_t bytes,const Receipt&receipt,uint32_t after,
                  uint32_t destination,std::vector<uint32_t>&out) {
 if(!r||bytes<ram_size||!receipt.qwords||receipt.qwords>1024u||
    !cpuRamAddress(receipt.before)||!cpuRamAddress(after)||
    (receipt.before&15u)||(after&15u)||after<=receipt.before||
    (receipt.before&0xe0000000u)!=(after&0xe0000000u)||
    after-receipt.before>16u*16u||(after-receipt.before)%16u||
    !receipt.source||(receipt.source&15u)||receipt.source>=ram_size||
    receipt.qwords*16u>ram_size-receipt.source||destination>0x7ffu)return false;
 std::vector<uint32_t> transaction;
 uint32_t consumed=0;
 for(uint32_t at=receipt.before;at<after;at+=16u) {
  uint32_t tag=0,source=0,flush=0,mpg=0;
  if(!word(r,bytes,at,tag)||!word(r,bytes,at+4u,source)||
     !word(r,bytes,at+8u,flush)||!word(r,bytes,at+12u,mpg))return false;
  const uint32_t qwc=tag&0xffffu, instructions=(mpg>>16)&255u, target=mpg&0xffffu;
  if(!qwc||qwc>127u||tag!=(0x30000000u|qwc)||flush!=0x10000000u||
     (mpg>>24)!=0x4au||instructions!=qwc*2u||
     consumed>receipt.qwords||qwc>receipt.qwords-consumed||
     source!=receipt.source+consumed*16u||target!=destination+consumed*2u||
     target*8u+qwc*16u>0x4000u)return false;
  const uint32_t physical=source&0x1fffffffu;
  if(physical>ram_size||qwc*16u>ram_size-physical)return false;
  transaction.push_back(flush);transaction.push_back(mpg);
  const size_t old=transaction.size();transaction.resize(old+qwc*4u);
  std::memcpy(transaction.data()+old,r+physical,qwc*16u);
  consumed+=qwc;
 }
 if(consumed!=receipt.qwords)return false;
 out=std::move(transaction);return true;
}
// enqueue/drain/invoke/take are EE-thread-only. Reset runs on that thread (or
// while stopped); replay is the renderer-thread operation protected against reset.
class Capture {
 std::atomic<uint64_t> epoch_{1};
 std::mutex replayMutex_;
 std::vector<uint32_t> pending_;
 std::vector<Receipt> receipts_;
public:
 uint64_t epoch()const{return epoch_.load(std::memory_order_acquire);}
 size_t pendingWords()const{return pending_.size();}
 size_t receiptCount()const{return receipts_.size();}
 void reset(){std::lock_guard<std::mutex> lock(replayMutex_);pending_.clear();receipts_.clear();epoch_.fetch_add(1,std::memory_order_acq_rel);}
 bool enqueue(uint64_t generation,std::vector<uint32_t> words) {
  if(generation!=epoch())return false;
  if(words.size()>max_words||pending_.size()>max_words-words.size()){reset();return false;}
  pending_.insert(pending_.end(),words.begin(),words.end());return true;
 }
 bool drain(Prefix&prefix) {
  const uint64_t current=epoch();
  if(prefix.epoch!=current){prefix.words.clear();prefix.epoch=current;}
  if(prefix.words.size()>max_words||pending_.size()>max_words-prefix.words.size()) {
   reset();prefix={};return false;
  }
  prefix.words.insert(prefix.words.end(),pending_.begin(),pending_.end());pending_.clear();return true;
 }
 bool valid(const Prefix&prefix)const{return prefix.epoch==epoch();}
 template<class Replay> bool replay(Prefix&prefix,Replay&&apply) {
  // A reset cannot complete while an old prefix is being applied, nor can an
  // old queued prefix start after reset. apply is the CPU VIF parser only;
  // it must not call guest code or re-enter Capture.
  std::lock_guard<std::mutex> lock(replayMutex_);
  const bool accepted=valid(prefix)&&!prefix.words.empty();
  if(accepted)apply(prefix.words);
  prefix={};return accepted;
 }
 // The same helper controls real hook invocation and yielded completion tests.
 template<class Native,class Complete,class Finish>
 void invoke(const Receipt*receipt,Native&&native,Complete&&complete,Finish&&finish) {
  native(); // exactly once, including a rejected capture
  if(!receipt||receipt->epoch!=epoch())return;
  if(complete(*receipt)){finish(*receipt);return;}
  if(receipts_.size()>=max_receipts)return;
  for(const Receipt&old:receipts_)if(old.thread==receipt->thread&&old.sp==receipt->sp)return;
  receipts_.push_back(*receipt);
 }
 bool take(int thread,uint32_t sp,uint32_t pc,Receipt&out) {
  for(size_t i=0;i<receipts_.size();++i) {
   const Receipt&entry=receipts_[i];
   if(entry.thread!=thread||entry.sp!=sp||entry.ra!=pc)continue;
   out=entry;receipts_.erase(receipts_.begin()+static_cast<std::ptrdiff_t>(i));
   return out.epoch==epoch();
  }
  return false;
 }
};
}
