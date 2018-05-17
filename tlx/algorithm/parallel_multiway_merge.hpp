/*******************************************************************************
 * tlx/algorithm/parallel_multiway_merge.hpp
 *
 * Parallel multiway merge.
 *
 * Copied and modified from STXXL, see http://stxxl.org, which itself extracted
 * it from MCSTL http://algo2.iti.uni-karlsruhe.de/singler/mcstl/. Both are
 * distributed under the Boost Software License, Version 1.0.
 *
 * Part of tlx - http://panthema.net/tlx
 *
 * Copyright (C) 2007 Johannes Singler <singler@ira.uka.de>
 * Copyright (C) 2014-2018 Timo Bingmann <tb@panthema.net>
 *
 * All rights reserved. Published under the Boost Software License, Version 1.0
 ******************************************************************************/

#ifndef TLX_ALGORITHM_PARALLEL_MULTIWAY_MERGE_HEADER
#define TLX_ALGORITHM_PARALLEL_MULTIWAY_MERGE_HEADER

#include <algorithm>
#include <thread>
#include <vector>

#if defined(_OPENMP)
#include <omp.h>
#endif

#include <tlx/algorithm/multiway_merge.hpp>
#include <tlx/algorithm/multiway_merge_splitting.hpp>
#include <tlx/simple_vector.hpp>

namespace tlx {

//! \addtogroup tlx_algorithm
//! \{

/*!
 * Different splitting strategies for sorting/merging: by sampling, exact
*/
enum MultiwayMergeSplittingAlgorithm {
    MWMSA_SAMPLING,
    MWMSA_EXACT,
    MWMSA_LAST,
    MWMSA_DEFAULT = MWMSA_EXACT
};

/*!
 * Parallel multi-way merge routine.
 *
 * Implemented either using OpenMP or with std::threads, depending on if
 * compiled with -fopenmp or not. The OpenMP version uses the implicit thread
 * pool, which is faster when using this method often.
 *
 * \param seqs_begin Begin iterator of iterator pair input sequence.
 * \param seqs_end End iterator of iterator pair input sequence.
 * \param target Begin iterator out output sequence.
 * \param size Maximum size to merge.
 * \param comp Comparator.
 * \param mwma MultiwayMergeAlgorithm set to use.
 * \param mwmsa MultiwayMergeSplittingAlgorithm to use.
 * \param num_threads Number of threads to use (defaults to all cores)
 * \tparam Stable Stable merging incurs a performance penalty.
 * \return End iterator of output sequence.
 */
template <
    bool Stable,
    typename RandomAccessIteratorIterator,
    typename RandomAccessIterator3,
    typename Comparator = std::less<
        typename std::iterator_traits<
            typename std::iterator_traits<RandomAccessIteratorIterator>
            ::value_type::first_type>::value_type> >
RandomAccessIterator3 parallel_multiway_merge_base(
    RandomAccessIteratorIterator seqs_begin,
    RandomAccessIteratorIterator seqs_end,
    RandomAccessIterator3 target,
    const typename std::iterator_traits<
        typename std::iterator_traits<
            RandomAccessIteratorIterator>::value_type::first_type>::
    difference_type size,
    Comparator comp = Comparator(),
    MultiwayMergeAlgorithm mwma = MWMA_ALGORITHM_DEFAULT,
    MultiwayMergeSplittingAlgorithm mwmsa = MWMSA_DEFAULT,
    size_t num_threads = std::thread::hardware_concurrency()) {

    using RandomAccessIteratorPair =
              typename std::iterator_traits<RandomAccessIteratorIterator>
              ::value_type;
    using RandomAccessIterator =
              typename RandomAccessIteratorPair::first_type;
    using DiffType = typename std::iterator_traits<RandomAccessIterator>
                     ::difference_type;

    // leave only non-empty sequences
    std::vector<RandomAccessIteratorPair> seqs_ne;
    seqs_ne.reserve(static_cast<size_t>(seqs_end - seqs_begin));
    DiffType total_size = 0;

    for (RandomAccessIteratorIterator raii = seqs_begin; raii != seqs_end; ++raii)
    {
        if (raii->first != raii->second) {
            total_size += raii->second - raii->first;
            seqs_ne.push_back(*raii);
        }
    }

    size_t num_seqs = seqs_ne.size();

    if (total_size == 0 || num_seqs == 0)
        return target;

    if (static_cast<DiffType>(num_threads) > total_size)
        num_threads = total_size;

    // thread t will have to merge chunks[iam][0..k - 1]

    simple_vector<std::vector<RandomAccessIteratorPair> > chunks(num_threads);

    for (size_t s = 0; s < num_threads; ++s)
        chunks[s].resize(num_seqs);

    if (mwmsa == MWMSA_SAMPLING)
    {
        multiway_merge_sampling_splitting<Stable>(
            seqs_ne.begin(), seqs_ne.end(),
            static_cast<DiffType>(size), total_size, comp,
            chunks.data(), num_threads);
    }
    else // (mwmsa == MWMSA_EXACT)
    {
        multiway_merge_exact_splitting<Stable>(
            seqs_ne.begin(), seqs_ne.end(),
            static_cast<DiffType>(size), total_size, comp,
            chunks.data(), num_threads);
    }

#if defined(_OPENMP)
#pragma omp parallel num_threads(num_threads)
    {
        size_t iam = omp_get_thread_num();

        DiffType target_position = 0, local_size = 0;

        for (size_t s = 0; s < num_seqs; ++s)
        {
            target_position += chunks[iam][s].first - seqs_ne[s].first;
            local_size += chunks[iam][s].second - chunks[iam][s].first;
        }

        multiway_merge_base<Stable, false>(
            chunks[iam].begin(), chunks[iam].end(),
            target + target_position,
            std::min(local_size, static_cast<DiffType>(size) - target_position),
            comp, mwma);
    }
#else
    std::vector<std::thread> threads(num_threads);

    for (size_t iam = 0; iam < num_threads; ++iam) {
        threads[iam] = std::thread(
            [&, iam]() {
                DiffType target_position = 0, local_size = 0;

                for (size_t s = 0; s < num_seqs; ++s)
                {
                    target_position += chunks[iam][s].first - seqs_ne[s].first;
                    local_size += chunks[iam][s].second - chunks[iam][s].first;
                }

                multiway_merge_base<Stable, false>(
                    chunks[iam].begin(), chunks[iam].end(),
                    target + target_position,
                    std::min(local_size, static_cast<DiffType>(size) - target_position),
                    comp, mwma);
            });
    }

    for (size_t i = 0; i < num_threads; ++i)
        threads[i].join();
#endif

    // update ends of sequences
    size_t count_seqs = 0;
    for (RandomAccessIteratorIterator raii = seqs_begin; raii != seqs_end; ++raii)
    {
        if (raii->first != raii->second)
            raii->first = chunks[num_threads - 1][count_seqs++].second;
    }

    return target + size;
}

/******************************************************************************/
// parallel_multiway_merge() Frontends

/*!
 * Parallel multi-way merge routine.
 *
 * Implemented either using OpenMP or with std::threads, depending on if
 * compiled with -fopenmp or not. The OpenMP version uses the implicit thread
 * pool, which is faster when using this method often.
 *
 * \param seqs_begin Begin iterator of iterator pair input sequence.
 * \param seqs_end End iterator of iterator pair input sequence.
 * \param target Begin iterator out output sequence.
 * \param size Maximum size to merge.
 * \param comp Comparator.
 * \param mwma MultiwayMergeAlgorithm set to use.
 * \param mwmsa MultiwayMergeSplittingAlgorithm to use.
 * \param num_threads Number of threads to use (defaults to all cores)
 * \tparam Stable Stable merging incurs a performance penalty.
 * \return End iterator of output sequence.
 */
template <
    typename RandomAccessIteratorIterator,
    typename RandomAccessIterator3,
    typename Comparator = std::less<
        typename std::iterator_traits<
            typename std::iterator_traits<RandomAccessIteratorIterator>
            ::value_type::first_type>::value_type> >
RandomAccessIterator3 parallel_multiway_merge(
    RandomAccessIteratorIterator seqs_begin,
    RandomAccessIteratorIterator seqs_end,
    RandomAccessIterator3 target,
    const typename std::iterator_traits<
        typename std::iterator_traits<
            RandomAccessIteratorIterator>::value_type::first_type>::
    difference_type size,
    Comparator comp = Comparator(),
    MultiwayMergeAlgorithm mwma = MWMA_ALGORITHM_DEFAULT,
    MultiwayMergeSplittingAlgorithm mwmsa = MWMSA_DEFAULT,
    size_t num_threads = std::thread::hardware_concurrency()) {

    return parallel_multiway_merge_base</* Stable */ false>(
        seqs_begin, seqs_end, target, size, comp, mwma, mwmsa, num_threads);
}

/*!
 * Parallel multi-way merge routine.
 *
 * Implemented either using OpenMP or with std::threads, depending on if
 * compiled with -fopenmp or not. The OpenMP version uses the implicit thread
 * pool, which is faster when using this method often.
 *
 * \param seqs_begin Begin iterator of iterator pair input sequence.
 * \param seqs_end End iterator of iterator pair input sequence.
 * \param target Begin iterator out output sequence.
 * \param size Maximum size to merge.
 * \param comp Comparator.
 * \param mwma MultiwayMergeAlgorithm set to use.
 * \param mwmsa MultiwayMergeSplittingAlgorithm to use.
 * \param num_threads Number of threads to use (defaults to all cores)
 * \tparam Stable Stable merging incurs a performance penalty.
 * \return End iterator of output sequence.
 */
template <
    typename RandomAccessIteratorIterator,
    typename RandomAccessIterator3,
    typename Comparator = std::less<
        typename std::iterator_traits<
            typename std::iterator_traits<RandomAccessIteratorIterator>
            ::value_type::first_type>::value_type> >
RandomAccessIterator3 stable_parallel_multiway_merge(
    RandomAccessIteratorIterator seqs_begin,
    RandomAccessIteratorIterator seqs_end,
    RandomAccessIterator3 target,
    const typename std::iterator_traits<
        typename std::iterator_traits<
            RandomAccessIteratorIterator>::value_type::first_type>::
    difference_type size,
    Comparator comp = Comparator(),
    MultiwayMergeAlgorithm mwma = MWMA_ALGORITHM_DEFAULT,
    MultiwayMergeSplittingAlgorithm mwmsa = MWMSA_DEFAULT,
    size_t num_threads = std::thread::hardware_concurrency()) {

    return parallel_multiway_merge_base</* Stable */ true>(
        seqs_begin, seqs_end, target, size, comp, mwma, mwmsa, num_threads);
}

//! \}

} // namespace tlx

#endif // !TLX_ALGORITHM_PARALLEL_MULTIWAY_MERGE_HEADER

/******************************************************************************/
