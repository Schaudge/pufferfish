//I am not sure what are the things I would need soon
//let's go with what we have
#include <iostream>
#include <mutex>
#include <vector>
#include <random>
#include <unordered_map>
#include <fstream>
#include <algorithm>
#include <iterator>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <thread>
#include <tuple>
#include <sstream>
#include <fstream>
#include <iostream>
#include <tuple>
#include <memory>
#include <cstring>
#include <queue>

//we already have timers

//cereal include
#include <cereal/types/unordered_map.hpp>
#include <cereal/types/vector.hpp>
#include <cereal/types/string.hpp>
#include <cereal/archives/binary.hpp>
#include <cereal/archives/json.hpp>
#include <sparsepp/spp.h>


#include "spdlog/spdlog.h"
#include "spdlog/sinks/ostream_sink.h"
#include "spdlog/sinks/stdout_sinks.h"
#include "spdlog/sinks/ansicolor_sink.h"
#include "spdlog/fmt/ostr.h"
#include "spdlog/fmt/fmt.h"


//jellyfish 2 include
#include "FastxParser.hpp"
#include "jellyfish/mer_dna.hpp"

//index header
#include "ProgOpts.hpp"
#include "PufferfishIndex.hpp"
#include "PufferfishSparseIndex.hpp"
#include "PufferfishLossyIndex.hpp"
#include "ScopedTimer.hpp"
#include "Util.hpp"
#include "SpinLock.hpp"
#include "MemCollector.hpp"
#include "SAMWriter.hpp"
#include "RefSeqConstructor.hpp"
#include "KSW2Aligner.hpp"
#include "zstr/zstr.hpp"

#define START_CONTIG_ID ((uint32_t)-1) 
#define END_CONTIG_ID ((uint32_t)-2)

#define MATCH_SCORE 1
#define MISMATCH_SCORE -1
#define GAP_SCORE -1

#define EPS 5

using paired_parser = fastx_parser::FastxParser<fastx_parser::ReadPair>;
using single_parser = fastx_parser::FastxParser<fastx_parser::ReadSeq>;

using HitCounters = util::HitCounters ;
using QuasiAlignment = util::QuasiAlignment ;
using MateStatus = util::MateStatus ;

using SpinLockT = std::mutex ;


void joinReadsAndFilter(spp::sparse_hash_map<size_t,std::vector<util::MemCluster>>& leftMemClusters,
                        spp::sparse_hash_map<size_t, std::vector<util::MemCluster>>& rightMemClusters,
                        std::vector<util::JointMems>& jointMemsList,
                        uint32_t maxFragmentLength,
                        uint32_t perfectCoverage,
                        double coverageRatio,
                        bool noDiscordant,
                        bool noOrphans,
                        bool verbose=false) {
  //orphan reads should be taken care of maybe with a flag!
  //uint32_t perfectCoverage{2*readLen};
  uint32_t maxCoverage{0};
  short round{0};
  //std::cout << "txp count:" << leftMemClusters.size() << "\n";
  while (round == 0 || (round == 1 && !jointMemsList.size() && !noDiscordant)) {
    for (auto& leftClustItr : leftMemClusters) {
      // reference id
      size_t tid = leftClustItr.first;
      // left mem clusters
      auto& lClusts = leftClustItr.second;
      // right mem clusters for the same reference id
      auto& rClusts = rightMemClusters[tid];
      //if ((lClusts.size() > 5 || rClusts.size() > 5) && (lClusts.size()>0 && rClusts.size()>0))
      //std::cout << "\t" << tid << ": lClusts.size:" << lClusts.size() << " , rClusts.size:" << rClusts.size() << "\n";
      // Compare the left clusters to the right clusters to filter by positional constraints
      for (auto lclust =  lClusts.begin(); lclust != lClusts.end(); lclust++) {
        for (auto rclust =  rClusts.begin(); rclust != rClusts.end(); rclust++) {
          // if both the left and right clusters are oriented in the same direction, skip this pair
          // NOTE: This should be optional as some libraries could allow this.
          if (round == 0 && lclust->isFw == rclust->isFw) { // if priority 0, ends should be concordant
            continue;
          }

          // FILTER 1
          // filter read pairs based on the fragment length which is approximated by the distance between the left most start and right most hit end
          size_t fragmentLen = rclust->lastRefPos() + rclust->lastMemLen() - lclust->firstRefPos();
          if (lclust->firstRefPos() > rclust->firstRefPos()) {
            fragmentLen = lclust->lastRefPos() + lclust->lastMemLen() - rclust->firstRefPos();
          }

          // FILTERING fragments with size smaller than maxFragmentLength : default 100,000 
          // FILTER just in case of priority 0 (round 0)
          if (fragmentLen < maxFragmentLength || round > 0) {
            // This will add a new potential mapping. Coverage of a mapping for read pairs is left->coverage + right->coverage
            // If we found a perfect coverage, we would only add those mappings that have the same perfect coverage
            auto totalCoverage = lclust->coverage + rclust->coverage;
            if (totalCoverage >= coverageRatio * maxCoverage or totalCoverage == perfectCoverage ) {//}|| (lclust->coverage + rclust->coverage) == ) {
              jointMemsList.emplace_back(tid, lclust, rclust, fragmentLen);
              if (verbose) {
                std::cout <<"\ntid:"<<tid<<"\n";
                std::cout <<"left:" << lclust->isFw << " size:" << lclust->mems.size() << " cov:" << lclust->coverage << "\n";
                for (size_t i = 0; i < lclust->mems.size(); i++){
                  std::cout << "--- t" << lclust->mems[i].tpos << " r"
                            << lclust->mems[i].memInfo->rpos << " cid:"
                            << lclust->mems[i].memInfo->cid << " cpos: "
                            << lclust->mems[i].memInfo->cpos << " len:"
                            << lclust->mems[i].memInfo->memlen << " fw:"
                            << lclust->mems[i].memInfo->cIsFw << "\n";
                }
                std::cout << "\nright:" << rclust->isFw << " size:" << rclust->mems.size() << " cov:" << rclust->coverage << "\n";
                for (size_t i = 0; i < rclust->mems.size(); i++){
                  std::cout << "--- t" << rclust->mems[i].tpos << " r"
                            << rclust->mems[i].memInfo->rpos << " cid:"
                            << rclust->mems[i].memInfo->cid << " cpos: "
                            << rclust->mems[i].memInfo->cpos << " len:"
                            << rclust->mems[i].memInfo->memlen << " fw:"
                            << rclust->mems[i].memInfo->cIsFw << "\n";
                }
              }
              uint32_t currCoverage =  jointMemsList.back().coverage();
              if (maxCoverage < currCoverage) {
                maxCoverage = currCoverage;
              }
            }
          } else {
            break;
          }
        }
      }
    }
    round++;
  }
  // If couldn't find any pair and allowed to add orphans
  if (!jointMemsList.size() && !noOrphans) {
    //std::cout << "Orphans\n";
    auto orphanFiller = [&jointMemsList, &maxCoverage]
                        (spp::sparse_hash_map<size_t,std::vector<util::MemCluster>>& memClusters,
                        bool isLeft) {
      // fragmentLen is set to 0
      std::vector<util::MemCluster> fake;
      for (auto& clustItr : memClusters) {
        // reference id
        size_t tid = clustItr.first;
        // left mem clusters
        auto& Clusts = clustItr.second;
        for (auto clust =  Clusts.begin(); clust != Clusts.end(); clust++) {
          if (isLeft)
            jointMemsList.emplace_back(tid, clust, fake.end(), 0, MateStatus::PAIRED_END_LEFT);
          else
            jointMemsList.emplace_back(tid, fake.end(), clust, 0, MateStatus::PAIRED_END_RIGHT);
          uint32_t currCoverage =  jointMemsList.back().coverage();
          if (maxCoverage < currCoverage) {
            maxCoverage = currCoverage;
          }    
        }
      }
    };
    orphanFiller(leftMemClusters, true);
    orphanFiller(rightMemClusters, false);
  } 
  if (verbose) {
    std::cout << "\nBefore filter " << jointMemsList.size() << " maxCov:" << maxCoverage << "\n";
    std::cout << "\n" << jointMemsList.size() << " maxCov:" << maxCoverage << "\n";
  }
  // FILTER 2
  // filter read pairs that don't have enough base coverage (i.e. their coverage is less than half of the maximum coverage for this read)
  //double coverageRatio = 0.5;
  // if we've found a perfect match, we will erase any match that is not perfect
  if (maxCoverage == perfectCoverage) {
    jointMemsList.erase(std::remove_if(jointMemsList.begin(), jointMemsList.end(),
                                        [&maxCoverage](util::JointMems& pairedReadMems) -> bool {
                                          return pairedReadMems.coverage() != maxCoverage;
                                        }),
                         jointMemsList.end());
  }
  else {// ow, we will go with the heuristic that just keep those mappings that have at least half of the maximum coverage
    //std::cout << "no!\n";
    jointMemsList.erase(std::remove_if(jointMemsList.begin(), jointMemsList.end(),
                                     [&maxCoverage, coverageRatio](util::JointMems& pairedReadMems) -> bool {
                                       return pairedReadMems.coverage() < coverageRatio*maxCoverage ;
                                     }),
                      jointMemsList.end());
  }
  if (verbose) {
    std::cout << "\nFinal stat: " << jointMemsList.size() << "\n";
    std::cout << jointMemsList.size() << "\n";
  }
}

std::string extractReadSeq(const std::string readSeq, uint32_t rstart, uint32_t rend, bool isFw) {
  std::string subseq = readSeq.substr(rstart, rend-rstart);
  if (isFw)
    return subseq;
  return util::reverseComplement(subseq); //reverse-complement the substring
}

// decode CIGAR, just like : https://github.com/lh3/ksw2/blob/master/cli.c#L134
std::string cigar2str(const ksw_extz_t* ez){
  std::string cigar ;
  if(ez->n_cigar > 0){
    cigar.resize(ez->n_cigar*2) ;
    for(int i = 0 ; i < ez->n_cigar ; ++i){
      cigar += (std::to_string(ez->cigar[i]>>4) + "MID"[ez->cigar[i]&0xf]) ;
    }
  }
  return cigar ;
}

std::string calculateCigar (std::string& read,
                            std::string& ref,
                            ksw2pp::KSW2Aligner& aligner,
                            std::vector<util::MemCluster>::iterator clust,
                            bool verbose=true) {
  if (read.empty()) {
    clust->cigar += (std::to_string(ref.length())+"D") ;
    clust->score += ref.length() * GAP_SCORE;
  } else if (ref.empty()) {
    clust->cigar += (std::to_string(read.length())+"I") ;
    clust->score += read.length() * GAP_SCORE;
  }
  else {
    auto score = aligner(read.c_str(),
                         read.length(),
                         ref.c_str(),
                         ref.length(),
                         ksw2pp::EnumToType<ksw2pp::KSW2AlignmentType::GLOBAL>()) ;
    clust->score += score;
    if (verbose) {
      std::cout << "read str " << read << "\nref str " << ref << "\n";
      std::cout << "cigar before : " << clust->cigar << "\n";
    }
    clust->cigar += cigar2str(&aligner.result()) ;
    if(verbose) std::cout << "cigar after : " << clust->cigar << "\n";
  }
  if (verbose) std::cout << clust->cigar << "\n";
  return clust->cigar;
}

template <typename PufferfishIndexT>
util::ContigBlock getContigBlock(std::vector<util::UniMemInfo>::iterator memInfo,
                        PufferfishIndexT* pfi,
                        spp::sparse_hash_map<uint32_t, util::ContigBlock>* contigSeqCache) {
  if(contigSeqCache->find(memInfo->cid) != contigSeqCache->end()){
    return (*contigSeqCache)[memInfo->cid];
  }
  else
    (*contigSeqCache)[memInfo->cid] = {memInfo->cid,
                                        memInfo->cGlobalPos,
                                        memInfo->clen,
                                        pfi->getSeqStr(memInfo->cGlobalPos, memInfo->clen)};
  
  return (*contigSeqCache)[memInfo->cid];
}


template <typename PufferfishIndexT>
void createSeqPairs(PufferfishIndexT* pfi,
                    std::vector<util::MemCluster>::iterator clust,
                    fastx_parser::ReadSeq& read,
                    RefSeqConstructor<PufferfishIndexT>& refSeqConstructor,
                    spp::sparse_hash_map<uint32_t, util::ContigBlock>& contigSeqCache,
                    uint32_t tid,
                    ksw2pp::KSW2Aligner& aligner,
                    bool verbose,
                    bool naive=true){

  //(void)verbose;
  (void)naive;
  //std::string& readName = read.name ;
  std::string& readSeq = read.seq ;
  auto clustSize = clust->mems.size()  ;
  clust->score = 0;
  auto readLen = readSeq.length() ;

  //@debug
  if(verbose) std::cout << "Clust size "<<clustSize<<"\n" ;

  clust->cigar = "" ;
  auto prevTPos = clust->mems[0].tpos;
  size_t it = 0;
  //FIXME why we have failures in graph!! if valid, discard the whole hit, otherwise, find the bug
  for(it=0 ; it < clust->mems.size() -1; ++it) {
    auto mmTstart = clust->mems[it].tpos + clust->mems[it].memInfo->memlen;
    auto mmTend = clust->mems[it+1].tpos;

    // NOTE: Assume that consistency condition is met, merging the mems 
    if (mmTend < mmTstart) { // this means the not matched seq is empty. i.e. we have exact match in transition from txp_i to txp_(i+1)
      continue;
    }

    uint32_t rstart = (clust->mems[it].memInfo->rpos + clust->mems[it].memInfo->memlen);
    uint32_t rend = clust->mems[it+1].memInfo->rpos;

    if (!clust->isFw) {
      rstart = (clust->mems[it+1].memInfo->rpos + clust->mems[it+1].memInfo->memlen);
      rend = clust->mems[it].memInfo->rpos;
    }
    if (mmTend == mmTstart && rstart == rend) {
      continue;
    }

    clust->cigar += (std::to_string(mmTstart-prevTPos) + "M");
    clust->score += ((mmTstart-prevTPos) * MATCH_SCORE);
    prevTPos = clust->mems[it+1].tpos;

    if (mmTend == mmTstart) { //Insertion in read
      if (rend-rstart > 0) { //validity check TODO if passed should be removed for final version
          //std::string tmp = extractReadSeq(readSeq, rstart, rend, clust->isFw) ;
        std::string readSubstr = extractReadSeq(readSeq, rstart, rend, clust->isFw);
        std::string refSeq = "";
        calculateCigar(readSubstr, refSeq, aligner, clust, verbose);
      }
      else {
          std::cerr << "ERROR: in pufferfishAligner tstart = tend while rend < rstart\n" << read.name << "\n";
      }
    }
    else if (rstart == rend) { //deletion in read
      if (mmTend-mmTstart > 0) { //validity check TODO if passed should be removed for final version
        //std::string tmp = extractReadSeq(readSeq, rstart, rend, clust->isFw) ;
        clust->cigar += (std::to_string(mmTend-mmTstart)+"D") ;
        clust->score += (mmTend-mmTstart) * GAP_SCORE;
        //calculateCigar(extractReadSeq(readSeq, rstart, rend, clust->isFw), "", aligner, clust);
      }
      else {
        std::cerr << "ERROR: in pufferfishAligner rstart = rend while tend < tstart\n" << read.name << "\n";
      }
    }
    else { // complex case
      // Need to fetch reference string --> Doing graph traversal
      std::string refSeq = "";
      bool firstContigDirWRTref = clust->mems[it].memInfo->cIsFw == clust->isFw; // true ~ (ref == contig)
      bool secondContigDirWRTref = clust->mems[it+1].memInfo->cIsFw == clust->isFw;
      auto distOnTxp = clust->mems[it+1].tpos - (clust->mems[it].tpos + clust->mems[it].memInfo->memlen);
      uint32_t cstart = firstContigDirWRTref?(clust->mems[it].memInfo->cpos + clust->mems[it].memInfo->memlen-1):clust->mems[it].memInfo->cpos;
      uint32_t cend = secondContigDirWRTref?clust->mems[it+1].memInfo->cpos:(clust->mems[it+1].memInfo->cpos + clust->mems[it+1].memInfo->memlen-1);

      // heuristic : length of mismatched parts of txp and read are both one??? It's a definite mismatch!!
      if (distOnTxp == 1 && (rend - rstart) == 1) {
        clust->cigar += "1M";
        clust->score += MISMATCH_SCORE;
      }

      else {

        util::ContigBlock scb = getContigBlock(clust->mems[it].memInfo, pfi, &contigSeqCache);
        util::ContigBlock ecb = getContigBlock(clust->mems[it+1].memInfo, pfi, &contigSeqCache);

        //NOTE In simplest form, assuming both start and end contigs are forward wrt the reference, then
        // cstart and cend point to the last matched base in the start contig and first matched base in last contig
        if (verbose) {
          std::cout << " start contig: \ncid" << scb.contigIdx_ << " relpos" << clust->mems[it].memInfo->cpos << " len" << scb.contigLen_ << " ori" << firstContigDirWRTref
                << " hitlen" << clust->mems[it].memInfo->memlen << " start pos:" << cstart << "\n" << scb.seq << "\n";
          std::cout << " last contig: \ncid" << ecb.contigIdx_ << " relpos" << clust->mems[it+1].memInfo->cpos << " len" << ecb.contigLen_ << " ori" << secondContigDirWRTref
                << " hitlen" << clust->mems[it+1].memInfo->memlen << " end pos:" << cend << "\n" << ecb.seq << "\n";
        }
        Task res = refSeqConstructor.fillSeq(tid,
                                           clust->mems[it].tpos + clust->mems[it].memInfo->memlen-1,
                                firstContigDirWRTref,
                                scb, cstart, ecb, cend,
                                secondContigDirWRTref,
                                             distOnTxp,
                                             refSeq);
        if(res == Task::SUCCESS) {
          //std::cout << "SUCCESS\n";
          //std::cout << " part of read "<<extractReadSeq(readSeq, rstart, rend, clust->isFw)<<"\n"
          //         << " part of ref  " << refSeq << "\n";
          std::string readSubstr = extractReadSeq(readSeq, rstart, rend, clust->isFw);
          calculateCigar(readSubstr, refSeq, aligner, clust, verbose) ;
        } else{
          //FIXME discard whole hit!!!
          clust->score = std::numeric_limits<int>::min();
          //std::cout << "Graph searched FAILED \n" ;
        }
      }
    }
  }
  // update cigar and add matches for last unimem(s)
  // after the loop, "it" is pointing to the last unimem in the change
  clust->cigar += (std::to_string(clust->mems[it].tpos + clust->mems[it].memInfo->memlen-prevTPos) + "M");
  clust->score += ((clust->mems[it].tpos + clust->mems[it].memInfo->memlen-prevTPos) * MATCH_SCORE);


  // Take care of left and right gaps/mismatches
  util::ContigBlock dummy = {std::numeric_limits<uint64_t>::max(),0,0,"",true};

  bool lastContigDirWRTref = clust->mems[it].memInfo->cIsFw == clust->isFw;
  bool firstContigDirWRTref = clust->mems[0].memInfo->cIsFw == clust->isFw;


  auto endRem = readLen - (clust->mems[it].memInfo->rpos + clust->mems[it].memInfo->memlen);
  auto startRem = clust->mems[0].memInfo->rpos;

  std::string startReadSeq = "";
  std::string endReadSeq = "";
  if (clust->isFw) {
    startReadSeq = extractReadSeq(readSeq, 0, clust->mems[0].memInfo->rpos, clust->isFw);
    endReadSeq = extractReadSeq(readSeq, (clust->mems[it].memInfo->rpos + clust->mems[it].memInfo->memlen), readLen, clust->isFw);
  }

  if (!clust->isFw) {
    endRem = clust->mems[it].memInfo->rpos;
    startRem = readLen - (clust->mems[0].memInfo->rpos + clust->mems[0].memInfo->memlen);
    startReadSeq = extractReadSeq(readSeq, (clust->mems[0].memInfo->rpos + clust->mems[0].memInfo->memlen), readLen, clust->isFw);
    endReadSeq = extractReadSeq(readSeq, 0, clust->mems[it].memInfo->rpos, clust->isFw);
  }

  /*if (verbose) std::cout << firstContigDirWRTref << " " << startRem << " " << startReadSeq
                         << " last " << lastContigDirWRTref << " " << endRem << " " << endReadSeq
                       << "\n";
  */
  if (endRem > 0) {
    //std::cout << "RIIIGHT hangover\n";
    std::string refSeq = "";
    util::ContigBlock scb = getContigBlock(clust->mems[it].memInfo, pfi, &contigSeqCache);
    uint32_t cstart = lastContigDirWRTref?(clust->mems[it].memInfo->cpos + clust->mems[it].memInfo->memlen-1):clust->mems[it].memInfo->cpos;

    Task res = refSeqConstructor.fillSeq(tid,
                                       clust->mems[it].tpos + clust->mems[it].memInfo->memlen-1,
                                         lastContigDirWRTref,
                                         scb, cstart, dummy, 0,
                                         lastContigDirWRTref,
                                         endRem,
                                         refSeq,
                                         verbose);
    if(res == Task::SUCCESS) {
      if (verbose)
        std::cerr << "\n\n\nSUCCESS   end extension::: " << read.name << "\n" << "read: " << endReadSeq << " ref: " << refSeq << "\n"
                << "tid " << tid << " starting from " << clust->mems[it].tpos + clust->mems[it].memInfo->memlen << " endRem " << endRem << " cid " << scb.contigIdx_ << "\n" ;
      
      //std::cout << " part of read "<<endReadSeq<<"\n"
      //          << " part of ref  " << refSeq << "\n";
      calculateCigar(endReadSeq, refSeq, aligner, clust, verbose) ;
    } else{
      if (verbose)
        std::cerr << "\n\n\nFAILURE   end extension::: " << read.name << "\n" << "read: " << endReadSeq << " ref: " << refSeq << "\n"
                << "tid " << tid << " starting from " << clust->mems[it].tpos + clust->mems[it].memInfo->memlen << " endRem " << endRem << " cid " << scb.contigIdx_ << "\n" ;
      // discard whole hit!!!
      clust->score = std::numeric_limits<int>::min();
      //std::cout << "Graph searched FAILED \n" ;
    }
  }
  if (startRem > 0) {
    //std::cout << read.name << "\n";
    //std::cout << "LEEEFT hangover\n";
    std::string refSeq = "";
    util::ContigBlock ecb = getContigBlock(clust->mems[0].memInfo, pfi, &contigSeqCache);
    if (verbose) {
      std::cout << firstContigDirWRTref << " " << clust->mems[0].memInfo->cpos
                           << " " << (clust->mems[0].memInfo->cpos + clust->mems[0].memInfo->memlen-1) << "\n";
      std::cout << ecb.seq.length() << "   " << ecb.seq << "\n";
    }
    uint32_t cend = firstContigDirWRTref?clust->mems[0].memInfo->cpos:(clust->mems[0].memInfo->cpos + clust->mems[0].memInfo->memlen-1);

    Task res = refSeqConstructor.fillSeq(tid,
                                         clust->mems[0].tpos + clust->mems[0].memInfo->memlen-1,
                                         firstContigDirWRTref,
                                         dummy, 0, ecb, cend,
                                         firstContigDirWRTref,
                                         startRem,
                                         refSeq);
    //if (verbose) std::cout << "got here\n";
    if(res == Task::SUCCESS) {
      //std::cout << " part of read "<<startReadSeq<<"\n"
      //          << " part of ref  " << refSeq << "\n";
      calculateCigar(endReadSeq, refSeq, aligner, clust, verbose) ;
    } else {
      std::cerr << "beginning extension::: " << read.name << "\n" << "read: " << endReadSeq << " ref: " << refSeq << "\n";
      // discard whole hit!!!
      clust->score = std::numeric_limits<int>::min();
      //std::cout << "Graph searched FAILED \n" ;
    }
  }
  clust->isVisited = true;
  if (verbose) std::cout << read.name << " " << clust->isFw << " " << clust->mems.size() << "\n" << read.seq << "\nSCORE: " << clust->score << "\nCIGAR: " << clust->cigar << "\n" ;
}

template <typename ReadPairT ,typename PufferfishIndexT>
void traverseGraph(ReadPairT& rpair,
                   util::JointMems& hit,
                   PufferfishIndexT& pfi,
                   RefSeqConstructor<PufferfishIndexT>& refSeqConstructor,
                   spp::sparse_hash_map<uint32_t, util::ContigBlock>& contigSeqCache,
                   ksw2pp::KSW2Aligner& aligner,
                   bool verbose,
                   bool naive=false){

  size_t tid = hit.tid ;
  auto readLen = rpair.first.seq.length() ;
  if(verbose) std::cout << rpair.first.name << "\n" ;

  if (hit.isOrphan()) {
    if(!hit.orphanClust()->isVisited && hit.orphanClust()->coverage < readLen)
      createSeqPairs(&pfi, hit.orphanClust(), rpair.first, refSeqConstructor, contigSeqCache, tid, aligner, verbose, naive);
    else if (hit.orphanClust()->coverage == readLen) {
      hit.orphanClust()->score += hit.orphanClust()->coverage * MATCH_SCORE;
      hit.orphanClust()->cigar = std::to_string(hit.orphanClust()->coverage) + "M";
    }
  }
  else {
    if(!hit.leftClust->isVisited && hit.leftClust->coverage < readLen)
      createSeqPairs(&pfi, hit.leftClust, rpair.first, refSeqConstructor, contigSeqCache, tid, aligner, verbose, naive);
    else if (hit.leftClust->coverage == readLen) {
      hit.leftClust->score += hit.leftClust->coverage * MATCH_SCORE;
      hit.leftClust->cigar = std::to_string(hit.leftClust->coverage) + "M";
    }
      //goOverClust(pfi, hit.leftClust, rpair.first, contigSeqCache, tid, verbose) ;
    if(!hit.rightClust->isVisited && hit.rightClust->coverage < readLen)
      createSeqPairs(&pfi, hit.rightClust, rpair.second, refSeqConstructor, contigSeqCache, tid, aligner, verbose, naive);
    else if (hit.rightClust->coverage == readLen) {
      hit.rightClust->score += hit.rightClust->coverage * MATCH_SCORE;
      hit.rightClust->cigar = std::to_string(hit.rightClust->coverage) + "M";
    }
  }
    //goOverClust(pfi, hit.rightClust, rpair.second, contigSeqCache, tid, verbose) ;
}



template <typename PufferfishIndexT>
void processReadsPair(paired_parser* parser,
                     PufferfishIndexT& pfi,
                     SpinLockT* iomutex,
                     std::shared_ptr<spdlog::logger> outQueue,
                     HitCounters& hctr,
                     AlignmentOpts* mopts){
  MemCollector<PufferfishIndexT> memCollector(&pfi) ;

  //create aligner
  spp::sparse_hash_map<uint32_t, util::ContigBlock> contigSeqCache ;
  RefSeqConstructor<PufferfishIndexT> refSeqConstructor(&pfi, &contigSeqCache);

  //std::cout << "\n In process reads pair\n" ;
    //TODO create a memory layout to store
    //strings then will allocate alignment to them
    //acco/rdingly
  std::vector<std::string> refBlocks ;

  auto logger = spdlog::get("stderrLog") ;
  fmt::MemoryWriter sstream ;
  //size_t batchSize{2500} ;
  size_t readLen{0};
  size_t totLen{0};

  spp::sparse_hash_map<size_t, std::vector<util::MemCluster>> leftHits ;
  spp::sparse_hash_map<size_t, std::vector<util::MemCluster>> rightHits ;
  std::vector<util::JointMems> jointHits ;
  PairedAlignmentFormatter<PufferfishIndexT*> formatter(&pfi);
  util::QueryCache qc;

  //@fatemeh Initialize aligner ksw 
  ksw2pp::KSW2Config config ;
  ksw2pp::KSW2Aligner aligner(MATCH_SCORE, MISMATCH_SCORE);

  config.gapo = -1 * GAP_SCORE ;
  config.gape = -1 * GAP_SCORE ;
  config.bandwidth = -1 ;
  config.flag = KSW_EZ_RIGHT ;
  aligner.config() = config ;
  
  auto rg = parser->getReadGroup() ;
  while(parser->refill(rg)){
    for(auto& rpair : rg){
      readLen = rpair.first.seq.length() ;
      totLen = readLen + rpair.second.seq.length();
      //std::cout << readLen << "\n";
      bool verbose = false;// rpair.second.name == "read30055083/ENST00000302182;mate1:504-603;mate2:582-680";// "fake2";
      if(verbose) std::cerr << rpair.first.name << "\n";

      ++hctr.numReads ;

      jointHits.clear() ;
      leftHits.clear() ;
      rightHits.clear() ;
      memCollector.clear();

      //help me to debug, will deprecate later
      //std::cout << "\n first seq in pair " << rpair.first.seq << "\n" ;
      //std::cout << "\n second seq in pair " << rpair.second.seq << "\n" ;

      //std::cout << "\n going inside hit collector \n" ;
      //readLen = rpair.first.seq.length() ;
      bool lh =  memCollector(rpair.first.seq,
                             leftHits,
                             mopts->maxSpliceGap,
                             MateStatus::PAIRED_END_LEFT,
                             qc,
                             verbose
                             /*
                             mopts->consistentHits,
                             refBlocks*/) ;
      bool rh =  memCollector(rpair.second.seq,
                             rightHits,
                             mopts->maxSpliceGap,
                             MateStatus::PAIRED_END_RIGHT,
                             qc,
                             verbose
                             /*,
                             mopts->consistentHits,
                             refBlocks*/) ;
      
      hctr.numMappedAtLeastAKmer += (leftHits.size() || rightHits.size()) ? 1 : 0;
      //do intersection on the basis of
      //performance, or going towards selective alignment
      //otherwise orphan
      if(verbose){
        for(auto& l : leftHits){
          auto& lclust = l.second ;
          for(auto& clust : lclust)
            for(auto& m : clust.mems){
              std::cout << "before join "<<m.memInfo->cid << " cpos "<< m.memInfo->cpos<< "\n" ;
            }
        }
        for(auto& l : rightHits){
          auto& lclust = l.second ;
          for(auto& clust : lclust)
            for(auto& m : clust.mems){
              std::cout << "before join "<<m.memInfo->cid << " cpos "<< m.memInfo->cpos <<" len:"<<m.memInfo->memlen<< "\n" ;
            }
        }
      }

      // We also handle orphans inside this function
      //if(lh && rh){
        joinReadsAndFilter(leftHits, rightHits, jointHits, 
        mopts->maxFragmentLength, 
        totLen, 
        mopts->scoreRatio, 
        mopts->noDiscordant,
        mopts->noOrphan,
        verbose) ;
      /* } else{
        //ignore orphans for now
      } */
      
      
      //jointHits is a vector
      //this can be used for BFS
      //NOTE sanity check
      //void traverseGraph(std::string& leftReadSeq, std::string& rightReadSeq, util::JointMems& hit, PufferfishIndexT& pfi,   std::map<uint32_t, std::string>& contigSeqCache){
      /*
      for(auto& h: jointHits){
        for(auto& m : h.leftClust->mems){
          auto cSeq = pfi.getSeqStr(pfi.getGlobalPos(m.memInfo->cid)+m.memInfo->cpos, m.memInfo->memlen) ;
          auto tmp = rpair.first.seq.substr(m.memInfo->rpos, m.memInfo->memlen) ;
          auto rseq = m.memInfo->cIsFw?tmp:util::reverseComplement(tmp);
          if(cSeq != rseq){
            std::cout <<"rpos:"<<m.memInfo->rpos<<"\t"<<"c "<<static_cast<int>(m.memInfo->cid)<<"\t"<<"cpos:"<<m.memInfo->cpos<<"\t"<<m.memInfo->memlen<<" cfw: "<<int(m.memInfo->cIsFw)<<" isFw "<<h.leftClust->isFw<< " tpos: "<<m.tpos<<"\n" ;
            std::cout<<rpair.first.name << "\n" ;
            std::cout << cSeq << "\n" << rseq <<"\n" << tmp << "\n";
            std::exit(1) ;
          }
        }
        for(auto& m : h.rightClust->mems){
          auto cSeq = pfi.getSeqStr(pfi.getGlobalPos(m.memInfo->cid)+m.memInfo->cpos, m.memInfo->memlen) ;
          auto tmp = rpair.second.seq.substr(m.memInfo->rpos, m.memInfo->memlen) ;
          auto rseq = m.memInfo->cIsFw?tmp:util::reverseComplement(tmp);
          if(cSeq != rseq){
            std::cout <<"rpos:"<<m.memInfo->rpos<<"\t"<<"c "<<static_cast<int>(m.memInfo->cid)<<"\t"<<"cpos:"<<m.memInfo->cpos<<"\t"<<m.memInfo->memlen<<" cfw: "<<int(m.memInfo->cIsFw)<<" isFw "<<h.rightClust->isFw<<" tpos: "<<m.tpos<<"\n" ;
            std::cout<<rpair.second.name << "\n" ;
            std::cout << cSeq << "\n" << rseq <<"\n" << tmp << "\n" ;
            std::exit(1) ;
          }
        }

        }*/

      int maxScore = std::numeric_limits<int>::min();
      /*
      bool doTraverse = !mopts->justMap;
      if (doTraverse) {
        //TODO Have to make it per thread 
        //have to make write access thread safe
        //std::cout << "traversing graph \n" ;

        if(!jointHits.empty() && jointHits.front().coverage() < 2*readLen) {
          for(auto& hit : jointHits){
            traverseGraph(rpair, hit, pfi, refSeqConstructor, contigSeqCache, aligner, verbose) ;
            // update minScore across all hits
            if(hit.leftClust->score + hit.rightClust->score > maxScore) {
              maxScore = hit.leftClust->score + hit.rightClust->score;
            }
          }
        }
        else if(!jointHits.empty()){
          maxScore = 2 * readLen * MATCH_SCORE ;
          for(auto& jointHit : jointHits){
            jointHit.leftClust->score = jointHit.leftClust->coverage * MATCH_SCORE ;
            jointHit.rightClust->score = jointHit.rightClust->coverage * MATCH_SCORE;
            jointHit.leftClust->cigar = std::to_string(readLen)+"M" ;
            jointHit.rightClust->cigar = std::to_string(readLen)+"M" ;
          }

        }
      }
      */


      hctr.totHits += jointHits.size();
      hctr.peHits += jointHits.size();
      hctr.numMapped += !jointHits.empty() ? 1 : 0;
      if (mopts->noOrphan)
        hctr.numOfOrphans += jointHits.empty() && (lh || rh);
      else
        hctr.numOfOrphans += !jointHits.empty() && (jointHits.back().isOrphan()) ? 1 : 0;

      if (jointHits.size() > hctr.maxMultimapping) {
        hctr.maxMultimapping = jointHits.size();
      }
      std::vector<QuasiAlignment> jointAlignments;
      for (auto& jointHit : jointHits) {
        // FIXME : This part needs to be taken care of 
        // If graph returned failure for one of the ends --> should be investigated more.
        if (!mopts->justMap and 
            ( (!jointHit.isOrphan() and (jointHit.leftClust->score == std::numeric_limits<int>::min()
                                        or jointHit.rightClust->score == std::numeric_limits<int>::min()) )
              or
              (jointHit.isLeftAvailable() and jointHit.leftClust->score == std::numeric_limits<int>::min()) 
              or 
              (jointHit.isRightAvailable() and jointHit.rightClust->score == std::numeric_limits<int>::min()))
        ) {
          //std::cerr << "Failed: " << rpair.first.name << "\n";
          continue;
        }
        if(mopts->justMap or (static_cast<int>(jointHit.coverage()) == maxScore)) {
          if (jointHit.isOrphan()) {
            jointAlignments.emplace_back(jointHit.tid,           // reference id
                                      jointHit.orphanClust()->getTrFirstHitPos(),     // reference pos
                                      jointHit.orphanClust()->isFw ,     // fwd direction
                                      readLen, // read length
                                      jointHit.orphanClust()->cigar, // cigar string 
                                      jointHit.fragmentLen,       // fragment length
                                      false); 
            auto& qaln = jointAlignments.back();
            qaln.mateStatus = jointHit.mateStatus;
          }
          else {
            //std::cerr << "Selected: " << rpair.first.name << "\n";
            jointAlignments.emplace_back(jointHit.tid,           // reference id
                                        jointHit.leftClust->getTrFirstHitPos(),     // reference pos
                                        jointHit.leftClust->isFw ,     // fwd direction
                                        readLen, // read length
                                        jointHit.leftClust->cigar, // cigar string 
                                        jointHit.fragmentLen,       // fragment length
                                        true);         // properly paired
            // Fill in the mate info		         // Fill in the mate info
            auto& qaln = jointAlignments.back();
            qaln.mateLen = readLen;
            qaln.mateCigar = jointHit.rightClust->cigar ;
            qaln.matePos = jointHit.rightClust->getTrFirstHitPos();
            qaln.mateIsFwd = jointHit.rightClust->isFw;
            qaln.mateStatus = MateStatus::PAIRED_END_PAIRED;
          }
        }
      }

      hctr.totAlignment += jointAlignments.size();

      if(mopts->krakOut){
        writeAlignmentsToKrakenDump(rpair, /* formatter,  */jointHits, sstream);
      } else if(jointAlignments.size() > 0 and !mopts->noOutput){
        writeAlignmentsToStream(rpair, formatter, jointAlignments, sstream, mopts->writeOrphans, mopts->justMap) ;
      } else if (jointAlignments.size() == 0 and !mopts->noOutput) {
        writeUnmappedAlignmentsToStream(rpair, formatter, jointAlignments, sstream, mopts->writeOrphans, mopts->justMap) ;
      }




      // write them on cmd
      if (hctr.numReads > hctr.lastPrint + 1000000) {
        hctr.lastPrint.store(hctr.numReads.load());
        if (!mopts->quiet and iomutex->try_lock()) {
          if (hctr.numReads > 0) {
            std::cerr << "\r\r";
          }
          std::cerr << "saw " << hctr.numReads << " reads : "
                    << "pe / read = " << hctr.peHits / static_cast<float>(hctr.numReads)
                    << " : se / read = " << hctr.seHits / static_cast<float>(hctr.numReads) << ' ';
          iomutex->unlock();
        }
      }
    } // for all reads in this job



    // dump output
    if (!mopts->noOutput) {
      std::string outStr(sstream.str());
      // Get rid of last newline
      if (!outStr.empty()) {
        outStr.pop_back();
        outQueue->info(std::move(outStr));
      }
      sstream.clear();
    }

  } // processed all reads
}

//===========
// SINGLE END
//============
template <typename PufferfishIndexT>
void processReadsSingle(single_parser* parser,
                     PufferfishIndexT& pfi,
                     SpinLockT* iomutex,
                     std::shared_ptr<spdlog::logger> outQueue,
                     HitCounters& hctr,
                     AlignmentOpts* mopts){
  MemCollector<PufferfishIndexT> memCollector(&pfi) ;

  //create aligner
  spp::sparse_hash_map<uint32_t, util::ContigBlock> contigSeqCache ;
  RefSeqConstructor<PufferfishIndexT> refSeqConstructor(&pfi, &contigSeqCache);
  std::vector<std::string> refBlocks ;

  auto logger = spdlog::get("stderrLog") ;
  fmt::MemoryWriter sstream ;
  //size_t batchSize{2500} ;
  size_t readLen{0};
  size_t totLen{0};

  spp::sparse_hash_map<size_t, std::vector<util::MemCluster>> leftHits ;
  spp::sparse_hash_map<size_t, std::vector<util::MemCluster>> rightHits ;
  std::vector<util::JointMems> jointHits ;
  PairedAlignmentFormatter<PufferfishIndexT*> formatter(&pfi);
  util::QueryCache qc;

  //@fatemeh Initialize aligner ksw
  ksw2pp::KSW2Config config ;
  ksw2pp::KSW2Aligner aligner(MATCH_SCORE, MISMATCH_SCORE);

  config.gapo = -1 * GAP_SCORE ;
  config.gape = -1 * GAP_SCORE ;
  config.bandwidth = -1 ;
  config.flag = KSW_EZ_RIGHT ;
  aligner.config() = config ;

  auto rg = parser->getReadGroup() ;
  while(parser->refill(rg)){
    for(auto& read : rg){
      readLen = read.seq.length() ;
      totLen = readLen;
      bool verbose = false;
      if(verbose) std::cout << read.name << "\n";
      ++hctr.numReads ;

      jointHits.clear() ;
      leftHits.clear() ;
      memCollector.clear();

      bool lh = memCollector(read.seq,
                             leftHits,
                             mopts->maxSpliceGap,
                             MateStatus::SINGLE_END,
                             qc,
                             false //verbose
                             /*
                             mopts->consistentHits,
                             refBlocks*/) ;
      (void)lh;
      //do intersection on the basis of
      //performance, or going towards selective alignment
      //otherwise orphan
      if(verbose){
        for(auto& l : leftHits){
          auto& lclust = l.second ;
          for(auto& clust : lclust)
            for(auto& m : clust.mems){
              std::cout << "before join "<<m.memInfo->cid << " cpos "<< m.memInfo->cpos<< "\n" ;
            }
        }
      }

      // Filter left hits
      uint32_t maxCoverage{0};
      uint32_t perfectCoverage{static_cast<uint32_t>(totLen)};
      std::vector<std::pair<uint32_t, decltype(leftHits)::mapped_type::iterator>> validHits;
      validHits.reserve(2*leftHits.size());
      for (auto& l : leftHits) {
        auto& lclust = l.second;
        /* if (read.name == "spj_1622951_1623126_68819/2") {
              std::cout << "\ntid" << l.first << " " << pfi.refName(l.first) << " cluster size:" << lclust.size() << "\n";
              for (auto& clus : lclust) {
                std::cout << "mem size: " << clus.mems.size() << "\n";
                for (auto& mem : clus.mems) {
                  std::cout << "t" << mem.tpos << " r" << mem.memInfo->rpos << " cid" << mem.memInfo->cid << " -- ";
                }
                std::cout << "\n";
              }
        } */
        for (auto clustIt = lclust.begin(); clustIt != lclust.end(); ++clustIt) {
          if (clustIt->coverage > maxCoverage) { maxCoverage = clustIt->coverage;}
          if (clustIt->coverage >= mopts->scoreRatio * maxCoverage or clustIt->coverage == perfectCoverage ) {
            validHits.emplace_back(static_cast<uint32_t>(l.first), clustIt);
            /*
            if (read.name == "spj_1622951_1623126_68819/2") {
              std::cout << "\ntid" << l.first << " " << pfi.refName(l.first) << " coverage:" << clustIt->coverage << "\n";
               for (auto& clus : lclust) {
                 std::cout << "mem size: " << clus.mems.size() << "\n";
                 for (auto& mem : clus.mems) {
                 std::cout << "t" << mem.tpos << " r" << mem.memInfo->rpos << " cid" << mem.memInfo->cid << " -- ";
                 }
                 std::cout << "\n";
                 }
            }
            */
          }
        }
        double thresh = mopts->scoreRatio * maxCoverage;
        // sort the hits by coverage
        std::sort(
            validHits.begin(), validHits.end(),
            [thresh](
                std::pair<uint32_t, decltype(leftHits)::mapped_type::iterator>&
                    e1, std::pair<uint32_t, decltype(leftHits)::mapped_type::iterator>& e2) -> bool {
              return e1.second->coverage > e2.second->coverage;
            });
        // remove those that don't achieve the threshold
        validHits.erase(std::remove_if(validHits.begin(), validHits.end(),
          [thresh](std::pair<uint32_t, decltype(leftHits)::mapped_type::iterator>& e) -> bool {
            return static_cast<double>(e.second->coverage) < thresh;
          }), validHits.end());
      }

      /*
      int maxScore = std::numeric_limits<int>::min();
      bool doTraverse = !mopts->justMap;
      if (doTraverse) {
        if(!jointHits.empty() && jointHits.front().coverage() < 2*readLen) {
          for(auto& hit : jointHits){
            traverseGraph(rpair, hit, pfi, refSeqConstructor, contigSeqCache, aligner, verbose) ;
            // update minScore across all hits
            if(hit.leftClust->score + hit.rightClust->score > maxScore) {
              maxScore = hit.leftClust->score + hit.rightClust->score;
            }
          }
        }
        else if(!jointHits.empty()){
          maxScore = 2 * readLen * MATCH_SCORE ;
          for(auto& jointHit : jointHits){
            jointHit.leftClust->score = jointHit.leftClust->coverage * MATCH_SCORE ;
            jointHit.rightClust->score = jointHit.rightClust->coverage * MATCH_SCORE;
            jointHit.leftClust->cigar = std::to_string(readLen)+"M" ;
            jointHit.rightClust->cigar = std::to_string(readLen)+"M" ;
          }

        }
      }
      */
      hctr.totHits += validHits.size();
      hctr.seHits += validHits.size();
      hctr.numMapped += !validHits.empty() ? 1 : 0;
      if (validHits.size() > hctr.maxMultimapping) {
        hctr.maxMultimapping = validHits.size();
      }
      std::vector<QuasiAlignment> jointAlignments;
      for (auto& hit : validHits) {
        // reference id
        size_t tid = hit.first;
        auto memIt = hit.second;
        jointAlignments.emplace_back(tid,                   // reference id
                                     memIt->firstRefPos(), // reference pos
                                     memIt->isFw,          // fwd direction
                                     readLen,               // read length
                                     memIt->cigar,          // cigar string
                                     readLen,               // fragment length
                                     false);                // properly paired
        // Fill in the mate info		         // Fill in the mate
        // info
        auto& qaln = jointAlignments.back();
        qaln.mateLen = readLen;
        qaln.mateCigar = "";
        qaln.matePos = 0;       // jointHit.rightClust->getTrFirstHitPos();
        qaln.mateIsFwd = false; // jointHit.rightClust->isFw;
        qaln.mateStatus = MateStatus::SINGLE_END;
      }

      hctr.totAlignment += validHits.size();

      // write puffkrak format output
      if(mopts->krakOut){
        writeAlignmentsToKrakenDump(read, /* formatter,  */validHits, sstream);
      } else if (validHits.size() > 0 and !mopts->noOutput) {
        // write sam output for mapped reads
        writeAlignmentsToStreamSingle(read, formatter, jointAlignments, sstream,
                               mopts->writeOrphans, mopts->justMap);
      } else if (validHits.size() == 0 and !mopts->noOutput) {
        // write sam output for un-mapped reads
        writeUnmappedAlignmentsToStreamSingle(read, formatter, jointAlignments,
                                        sstream, mopts->writeOrphans,
                                        mopts->justMap);
      }

      // write them on cmd
      if (hctr.numReads > hctr.lastPrint + 1000000) {
        hctr.lastPrint.store(hctr.numReads.load());
        if (!mopts->quiet and iomutex->try_lock()) {
          if (hctr.numReads > 0) {
            std::cerr << "\r\r";
          }
          std::cerr << "saw " << hctr.numReads << " reads : "
                    << "pe / read = "
                    << hctr.peHits / static_cast<float>(hctr.numReads)
                    << " : se / read = "
                    << hctr.seHits / static_cast<float>(hctr.numReads) << ' ';
          iomutex->unlock();
        }
      }
    } // for all reads in this job


    // dump output
    if (!mopts->noOutput) {
      std::string outStr(sstream.str());
      // Get rid of last newline
      if (!outStr.empty()) {
        outStr.pop_back();
        outQueue->info(std::move(outStr));
      }
      sstream.clear();
    }

  } // processed all reads
}

template <typename PufferfishIndexT>
bool spawnProcessReadsthreads(
                              uint32_t nthread,
                              paired_parser* parser,
                              PufferfishIndexT& pfi,
                              SpinLockT& iomutex,
                              std::shared_ptr<spdlog::logger> outQueue,
                              HitCounters& hctr,
                              AlignmentOpts* mopts){

  std::vector<std::thread> threads ;

  for(size_t i = 0; i < nthread ; ++i){

    threads.emplace_back(processReadsPair<PufferfishIndexT>,
                         parser,
                         std::ref(pfi),
                         &iomutex,
                         outQueue,
                         std::ref(hctr),
                         mopts);
  }
  for(auto& t : threads) { t.join(); }

  return true ;
}

template <typename PufferfishIndexT>
bool spawnProcessReadsthreads(
                              uint32_t nthread,
                              single_parser* parser,
                              PufferfishIndexT& pfi,
                              SpinLockT& iomutex,
                              std::shared_ptr<spdlog::logger> outQueue,
                              HitCounters& hctr,
                              AlignmentOpts* mopts){

  std::vector<std::thread> threads ;

  for(size_t i = 0; i < nthread ; ++i){

    threads.emplace_back(processReadsSingle<PufferfishIndexT>,
                         parser,
                         std::ref(pfi),
                         &iomutex,
                         outQueue,
                         std::ref(hctr),
                         mopts);
  }
  for(auto& t : threads) { t.join(); }

  return true ;
}

void printAlignmentSummary(HitCounters& hctrs, std::shared_ptr<spdlog::logger> consoleLog) {
  consoleLog->info("Done mapping reads.");
  consoleLog->info("\n\n");
  consoleLog->info("=====");
  consoleLog->info("Observed {} reads", hctrs.numReads);
  consoleLog->info("Rate of Fragments with at least one found k-mer: {:03.2f}%", 
    (100.0 * static_cast<float>(hctrs.numMappedAtLeastAKmer)) / hctrs.numReads);
  consoleLog->info("Discordant Rate: {:03.2f}%", 
    (100.0 * static_cast<float>(hctrs.numOfOrphans)) / hctrs.numReads);
  consoleLog->info("Mapping rate : {:03.2f}%", (100.0 * static_cast<float>(hctrs.numMapped)) / hctrs.numReads);
  consoleLog->info("Average # hits per read : {}", hctrs.totHits / static_cast<float>(hctrs.numReads));
  consoleLog->info("Total # of alignments : {}", hctrs.totAlignment);
  consoleLog->info("Max multimapping group : {}", hctrs.maxMultimapping);
  consoleLog->info("=====");
}

template <typename PufferfishIndexT>
bool alignReads(
              PufferfishIndexT& pfi,
              std::shared_ptr<spdlog::logger> consoleLog,
              AlignmentOpts* mopts) {



  std::streambuf* outBuf ;
  std::ofstream outFile ;
  std::unique_ptr<std::ostream> outStream{nullptr};
  //bool haveOutputFile{false} ;
  std::shared_ptr<spdlog::logger> outLog{nullptr};
  if (!mopts->noOutput) {
    if(mopts->outname == ""){
      outBuf = std::cout.rdbuf() ;
    }else{
      outFile.open(mopts->outname) ;
      outBuf = outFile.rdbuf() ;
      //haveOutputFile = true ;
    }

    // out stream to the buffer
    // it can be std::cout or a file
    if (mopts->compressedOutput) {
	outStream.reset(new zstr::ostream(outBuf));
    } else {
    	outStream.reset(new std::ostream(outBuf));
    }
    // the async queue size must be a power of 2
    size_t queueSize{268435456};
    spdlog::set_async_mode(queueSize);

    auto outputSink = std::make_shared<spdlog::sinks::ostream_sink_mt>(*outStream) ;
    outLog = std::make_shared<spdlog::logger>("puffer::outLog",outputSink) ;
    outLog->set_pattern("%v");

    // write the SAM Header
    // If nothing gets printed by this time we are in trouble
    if (mopts->krakOut) {
      writeKrakOutHeader(pfi, outLog, mopts);
    } else {
      writeSAMHeader(pfi, outLog);
    }
  }

  uint32_t nthread = mopts->numThreads ;
  std::unique_ptr<paired_parser> pairParserPtr{nullptr} ;
  std::unique_ptr<single_parser> singleParserPtr{nullptr} ;

  size_t chunkSize{10000} ;
  SpinLockT iomutex ;
  if (!mopts->singleEnd)
  {
    ScopedTimer timer(!mopts->quiet) ;
    HitCounters hctrs ;
    consoleLog->info("mapping reads ... \n\n\n") ;
    std::vector<std::string> read1Vec = util::tokenize(mopts->read1, ',') ;
    std::vector<std::string> read2Vec = util::tokenize(mopts->read2, ',') ;

    if(read1Vec.size() != read2Vec.size()){
      consoleLog->error("The number of provided files for"
                        "-1 and -2 are not same!") ;
      std::exit(1) ;
    }

    uint32_t nprod = (read1Vec.size() > 1) ? 2 : 1;
    pairParserPtr.reset(new paired_parser(read1Vec, read2Vec, nthread, nprod, chunkSize));
    pairParserPtr->start();

    spawnProcessReadsthreads(nthread, pairParserPtr.get(), pfi, iomutex,
                             outLog, hctrs, mopts) ;

    pairParserPtr->stop();
    consoleLog->info("flushing output queue.");
    printAlignmentSummary(hctrs, consoleLog);
	if (outLog) { outLog->flush(); }
  } else {
    ScopedTimer timer(!mopts->quiet) ;
    HitCounters hctrs ;
    consoleLog->info("mapping reads ... \n\n\n") ;
    std::vector<std::string> readVec = util::tokenize(mopts->unmatedReads, ',') ;

    uint32_t nprod = (readVec.size() > 1) ? 2 : 1;
    singleParserPtr.reset(new single_parser(readVec, nthread, nprod, chunkSize));
    singleParserPtr->start();

    spawnProcessReadsthreads(nthread, singleParserPtr.get(), pfi, iomutex,
                             outLog, hctrs, mopts) ;

    singleParserPtr->stop();
    consoleLog->info("flushing output queue.");
    printAlignmentSummary(hctrs, consoleLog);
    if (outLog) { outLog->flush(); }
  }
  return true ;
}

int pufferfishAligner(AlignmentOpts& alnargs){

  auto consoleLog = spdlog::stderr_color_mt("console");
  bool success{false} ;
  auto indexDir = alnargs.indexDir ;

  std::string indexType;
  {
    std::ifstream infoStream(indexDir + "/info.json");
    cereal::JSONInputArchive infoArchive(infoStream);
    infoArchive(cereal::make_nvp("sampling_type", indexType));
    std::cout << "Index type = " << indexType << '\n';
    infoStream.close();
  }

  if(indexType == "dense"){
    PufferfishIndex pfi(indexDir) ;
    success = alignReads(pfi, consoleLog, &alnargs) ;

  }else if(indexType == "sparse"){
    PufferfishSparseIndex pfi(indexDir) ;
    success = alignReads(pfi, consoleLog, &alnargs) ;
  }else if(indexType == "lossy"){
    PufferfishLossyIndex pfi(indexDir);
    success = alignReads(pfi, consoleLog, &alnargs) ;
  }

  if (!success) {
    consoleLog->warn("Problem mapping.");
  }
  return 0 ;
}
