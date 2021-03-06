// COLMAP - Structure-from-Motion.
// Copyright (C) 2016  Johannes L. Schoenberger <jsch at inf.ethz.ch>
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.

#ifndef COLMAP_SRC_RETRIEVAL_INVERTED_FILE_H_
#define COLMAP_SRC_RETRIEVAL_INVERTED_FILE_H_

#include <algorithm>
#include <bitset>
#include <cstdint>
#include <fstream>
#include <unordered_set>
#include <vector>

#include <Eigen/Core>

#include "retrieval/inverted_file_entry.h"
#include "retrieval/utils.h"
#include "util/logging.h"
#include "util/math.h"

namespace colmap {
namespace retrieval {

// Implements an inverted file, including the ability to compute image scores
// and matches. The template parameter is the length of the binary vectors
// in the Hamming Embedding.
// This class is based on an original implementation by Torsten Sattler.
template <int N>
class InvertedFile {
 public:
  typedef Eigen::Matrix<float, N, 1> Desc;

  enum Status {
    UNUSABLE = 0x00,
    HAS_HAMMING_EMBEDDING = 0x01,
    ENTRIES_SORTED = 0x02,
    USABLE = 0x03,
  };

  InvertedFile();

  // The number of added entries.
  size_t NumEntries() const;

  // Whether the Hamming embedding was computed for this file.
  bool HasHammingEmbedding() const;

  // Whether the entries in this file are sorted.
  bool EntriesSorted() const;

  // Whether this file is usable for scoring, i.e. the entries are sorted and
  // the Hamming embedding has been computed.
  bool IsUsable() const;

  // Adds an inverted file entry given a projected descriptor and its image
  // information stored in an inverted file entry. In particular, this function
  // generates the binary descriptor for the inverted file entry and then stores
  // the entry in the inverted file.
  void AddEntry(const int image_id, const Desc& descriptor);

  // Sorts the inverted file entries in ascending order of image ids. This is
  // required for efficient scoring and must be called before ScoreFeature.
  void SortEntries();

  // Clear all entries in this file.
  void ClearEntries();

  // Reset all computed weights/thresholds and clear all entries.
  void Reset();

  // Given a projected descriptor, returns the corresponding binary string.
  void ConvertToBinaryDescriptor(const Desc& descriptor,
                                 std::bitset<N>* bin_desc) const;

  // Compute the idf-weight for this inverted file.
  void ComputeIDFWeight(const int num_total_images);

  // Return the idf-weight of this inverted file.
  float IDFWeight() const;

  // Given a set of descriptors, learns the thresholds required for the Hamming
  // embedding. Each row in descriptors represents a single descriptor projected
  // into the N dimensional space used for Hamming embedding.
  void ComputeHammingEmbedding(
      const Eigen::Matrix<float, Eigen::Dynamic, N>& descriptors);

  // Given a query feature, performs inverted file scoring.
  void ScoreFeature(const Desc& descriptor,
                    std::vector<ImageScore>* image_scores) const;

  // Get the identifiers of all indexed images in this file.
  void GetImageIds(std::unordered_set<int>* ids) const;

  // For each image in the inverted file, computes the self-similarity of each
  // image in the file (the part caused by this word) and adds the weight to the
  // entry corresponding to that image. This function is useful to determine the
  // normalization factor for each image that is used during retrieval.
  void ComputeImageSelfSimilarities(
      std::vector<double>* self_similarities) const;

  // Read/write the inverted file from/to a binary file.
  void Read(std::ifstream* ifs);
  void Write(std::ofstream* ofs) const;

 private:
  // Whether the inverted file is initialized.
  uint8_t status_;

  // The inverse document frequency weight of this inverted file.
  float idf_weight_;

  // The entries of the inverted file system.
  std::vector<InvertedFileEntry<N>> entries_;

  // The thresholds used for Hamming embedding.
  Desc thresholds_;

  // The functor to derive a voting weight from a Hamming distance.
  static const HammingDistWeightFunctor<N> hamming_dist_weight_functor_;
};

////////////////////////////////////////////////////////////////////////////////
// Implementation
////////////////////////////////////////////////////////////////////////////////

template <int N>
const HammingDistWeightFunctor<N> InvertedFile<N>::hamming_dist_weight_functor_;

template <int N>
InvertedFile<N>::InvertedFile() : status_(UNUSABLE), idf_weight_(0.0f) {
  static_assert(N % 8 == 0,
                "Dimensionality of projected space needs to"
                " be a multiple of 8.");
  static_assert(N > 0, "Dimensionality of projected space needs to be > 0.");

  thresholds_.setZero();
}

template <int N>
size_t InvertedFile<N>::NumEntries() const {
  return entries_.size();
}

template <int N>
bool InvertedFile<N>::HasHammingEmbedding() const {
  return status_ & HAS_HAMMING_EMBEDDING;
}

template <int N>
bool InvertedFile<N>::EntriesSorted() const {
  return status_ & ENTRIES_SORTED;
}

template <int N>
bool InvertedFile<N>::IsUsable() const {
  return status_ & USABLE;
}

template <int N>
void InvertedFile<N>::AddEntry(const int image_id, const Desc& descriptor) {
  CHECK_GE(image_id, 0);
  InvertedFileEntry<N> entry;
  entry.image_id = image_id;
  ConvertToBinaryDescriptor(descriptor, &entry.descriptor);
  entries_.push_back(entry);
  status_ &= ~ENTRIES_SORTED;
}

template <int N>
void InvertedFile<N>::SortEntries() {
  std::sort(entries_.begin(), entries_.end(),
            [](const InvertedFileEntry<N>& entry1,
               const InvertedFileEntry<N>& entry2) {
              return entry1.image_id < entry2.image_id;
            });
  status_ |= ENTRIES_SORTED;
}

template <int N>
void InvertedFile<N>::ClearEntries() {
  entries_.clear();
  status_ &= ~ENTRIES_SORTED;
}

template <int N>
void InvertedFile<N>::Reset() {
  status_ = UNUSABLE;
  idf_weight_ = 0.0f;
  entries_.clear();
  thresholds_.setZero();
}

template <int N>
void InvertedFile<N>::ConvertToBinaryDescriptor(
    const Desc& descriptor, std::bitset<N>* bin_desc) const {
  for (int i = 0; i < N; ++i) {
    (*bin_desc)[i] = descriptor[i] > thresholds_[i];
  }
}

template <int N>
void InvertedFile<N>::ComputeIDFWeight(const int num_total_images) {
  if (entries_.empty()) {
    return;
  }

  std::unordered_set<int> image_ids;
  GetImageIds(&image_ids);

  idf_weight_ = std::log(1.0 +
                         static_cast<double>(num_total_images) /
                             static_cast<double>(image_ids.size()));
}

template <int N>
float InvertedFile<N>::IDFWeight() const {
  return idf_weight_;
}

template <int N>
void InvertedFile<N>::ComputeHammingEmbedding(
    const Eigen::Matrix<float, Eigen::Dynamic, N>& descriptors) {
  const int num_descriptors = static_cast<int>(descriptors.rows());
  if (num_descriptors < 2) {
    return;
  }

  std::vector<float> elements(num_descriptors);
  for (int n = 0; n < N; ++n) {
    for (int i = 0; i < num_descriptors; ++i) {
      elements[i] = descriptors(i, n);
    }
    thresholds_[n] = Median(elements);
  }

  status_ |= HAS_HAMMING_EMBEDDING;
}

template <int N>
void InvertedFile<N>::ScoreFeature(
    const Desc& descriptor, std::vector<ImageScore>* image_scores) const {
  image_scores->clear();

  if (!IsUsable()) {
    return;
  }

  if (entries_.size() == 0) {
    return;
  }

  const float squared_idf_weight = idf_weight_ * idf_weight_;

  std::bitset<N> bin_descriptor;
  ConvertToBinaryDescriptor(descriptor, &bin_descriptor);

  ImageScore image_score;
  image_score.image_id = entries_.front().image_id;
  image_score.score = 0.0f;
  int num_image_votes = 0;

  // Note that this assumes that the entries are sorted using SortEntries
  // according to their image identifiers.
  for (const auto& entry : entries_) {
    if (image_score.image_id < entry.image_id) {
      if (num_image_votes > 0) {
        // Finalizes the voting since we now know how many features from
        // the database image match the current image feature. This is
        // required to perform burstiness normalization (cf. Eqn. 2 in
        // Arandjelovic, Zisserman: Scalable descriptor
        // distinctiveness for location recognition. ACCV 2014.
        // Notice that the weight from the descriptor matching is already
        // accumulated in image_score.score, i.e., we only need
        // to apply the burstiness weighting.
        image_score.score /= std::sqrt(static_cast<float>(num_image_votes));
        image_score.score *= squared_idf_weight;
        image_scores->push_back(image_score);
      }

      image_score.image_id = entry.image_id;
      image_score.score = 0.0f;
      num_image_votes = 0;
    }

    const size_t hamming_dist = (bin_descriptor ^ entry.descriptor).count();

    image_score.score += hamming_dist_weight_functor_(hamming_dist);
    num_image_votes += 1;
  }

  // Add the voting for the largest image_id in the entries.
  if (num_image_votes > 0) {
    image_score.score /= std::sqrt(static_cast<float>(num_image_votes));
    image_score.score *= squared_idf_weight;
    image_scores->push_back(image_score);
  }
}

template <int N>
void InvertedFile<N>::GetImageIds(std::unordered_set<int>* ids) const {
  for (const InvertedFileEntry<N>& entry : entries_) {
    ids->insert(entry.image_id);
  }
}

template <int N>
void InvertedFile<N>::ComputeImageSelfSimilarities(
    std::vector<double>* self_similarities) const {
  const double squared_idf_weight = idf_weight_ * idf_weight_;
  for (const auto& entry : entries_) {
    self_similarities->at(entry.image_id) += squared_idf_weight;
  }
}

template <int N>
void InvertedFile<N>::Read(std::ifstream* ifs) {
  CHECK(ifs->is_open());

  ifs->read(reinterpret_cast<char*>(&status_), sizeof(uint8_t));
  ifs->read(reinterpret_cast<char*>(&idf_weight_), sizeof(float));

  for (int i = 0; i < N; ++i) {
    ifs->read(reinterpret_cast<char*>(&thresholds_[i]), sizeof(float));
  }

  uint32_t num_entries = 0;
  ifs->read(reinterpret_cast<char*>(&num_entries), sizeof(uint32_t));
  entries_.resize(num_entries);

  for (uint32_t i = 0; i < num_entries; ++i) {
    entries_[i].Read(ifs);
  }
}

template <int N>
void InvertedFile<N>::Write(std::ofstream* ofs) const {
  CHECK(ofs->is_open());

  ofs->write(reinterpret_cast<const char*>(&status_), sizeof(uint8_t));
  ofs->write(reinterpret_cast<const char*>(&idf_weight_), sizeof(float));

  for (int i = 0; i < N; ++i) {
    ofs->write(reinterpret_cast<const char*>(&thresholds_[i]), sizeof(float));
  }

  const uint32_t num_entries = static_cast<uint32_t>(entries_.size());
  ofs->write(reinterpret_cast<const char*>(&num_entries), sizeof(uint32_t));

  for (uint32_t i = 0; i < num_entries; ++i) {
    entries_[i].Write(ofs);
  }
}

}  // namespace retrieval
}  // namespace colmap

#endif  // COLMAP_SRC_RETRIEVAL_INVERTED_FILE_H_
