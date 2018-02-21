#ifndef _PUFFERFISHBASE_INDEX_HPP_
#define _PUFFERFISHBASE_INDEX_HPP_

#include <vector>

#include "core/range.hpp"
#include "sdsl/int_vector.hpp"
#include "sdsl/rank_support.hpp"
#include "sdsl/select_support.hpp"
#include "cereal/archives/json.hpp"

#include "CanonicalKmer.hpp"
#include "CanonicalKmerIterator.hpp"
#include "BooPHF.h"
#include "Util.hpp"
#include "PufferfishTypes.hpp"

template <typename T>
class PufferfishBaseIndex {
    private:
    // from : https://www.fluentcpp.com/2017/05/19/crtp-helper/
    T& underlying(); 
    T const& underlying() const;

  public:
  // Get the equivalence class ID (i.e., rank of the equivalence class)
  // for a given contig.
  pufferfish::types::EqClassID getEqClassID(uint32_t contigID);

  // Get the equivalence class label for a contig (i.e., the set of reference
  // sequences containing
  // the contig).
  const pufferfish::types::EqClassLabel& getEqClassLabel(uint32_t contigID);

  // Get the k value with which this index was built.
  uint32_t k();

  // Get the list of reference sequences & positiosn corresponding to a contig
  const std::vector<util::Position>& refList(uint64_t contigRank);
  
  // Get the name of a given reference sequence
  const std::string& refName(uint64_t refRank);
  
  // Get the length of a reference sequence
  uint32_t refLength(uint64_t refRank) const;

  // Get the list of reference names
  const std::vector<std::string>& getRefNames() ;
  // and lengths
  const std::vector<uint32_t>& getRefLengths() const;

  // Returns true if pos is a valid position in the compacted sequence array
  // and false otherwise.
  bool isValidPos(uint64_t pos);
  
  // Returns a ProjectedHits object that contains all of the
  // projected reference hits for the given kmer.
  auto getRefPos(CanonicalKmer& mer) -> util::ProjectedHits;

  // Returns the string value of contig sequence vector starting from position `globalPos` with `length` bases
  // and reverse-complements the string if `isFw` is false
  std::string getSeqStr(size_t globalPos, size_t length, bool isFw=true);

  // Returns a ProjectedHits object that contains all of the
  // projected reference hits for the given kmer.  Uses the results
  // of the previous contig info (start, end) from qc if the same
  // contig contains the match.  For correlated searches (e.g., from a read)
  // this can considerably speed up querying.
  auto getRefPos(CanonicalKmer& mer, util::QueryCache& qc) -> util::ProjectedHits;

  sdsl::int_vector<2>& getSeq(); 
  sdsl::int_vector<8>& getEdge(); 

  uint8_t getEdgeEntry(uint64_t contigRank) const;
  //uint8_t getRevEdgeEntry(uint64_t contigRank) {return revedge_[contigRank];}
  CanonicalKmer getStartKmer(uint64_t cid) ;
  CanonicalKmer getEndKmer(uint64_t cid) ;
  uint32_t getContigLen(uint64_t cid) ;
  uint64_t getGlobalPos(uint64_t cid) ;
  auto  getContigBlock(uint64_t rank) -> util::ContigBlock ;
  
  // Returns true if the given k-mer appears in the dBG, false otherwise
  /*
  bool contains(CanonicalKmer& mer);

  // Returns the contigID associated with the k-mer mer
  uint32_t contigID(CanonicalKmer& mer);

  // Returns the position in the compacted dBG sequence vector where the
  // given k-mer occurs, or std::numeric_limits<uint64_t>::max() otherwise.
  uint64_t getRawPos(CanonicalKmer& mer);
  */
  /*
  std::vector<CanonicalKmer> getNextKmerOnGraph(uint64_t cid, util::Direction dir, bool isCurContigFwd);
  */
  //void getRawSeq(util::ProjectedHits& phits, CanonicalKmerIterator& kit, std::string& contigStr, int readLen);
};

#endif // _PUFFERFISHBASE_INDEX_HPP_