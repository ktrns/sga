///-----------------------------------------------
// Copyright 2010 Wellcome Trust Sanger Institute
// Written by Jared Simpson (js18@sanger.ac.uk)
// Released under the GPL
//-----------------------------------------------
//
// ErrorCorrectProcess - Wrapper to perform error correction
// for a sequence work item
//
#include "ErrorCorrectProcess.h"
#include "ErrorCorrect.h"
#include "CorrectionThresholds.h"

//#define KMER_TESTING 1

//
//
//
ErrorCorrectProcess::ErrorCorrectProcess(const OverlapAlgorithm* pOverlapper, 
                                         int minOverlap, 
                                         int numOverlapRounds,
                                         int numKmerRounds,
                                         int conflictCutoff,
                                         int kmerLength,
                                         int kmerThreshold, 
                                         ErrorCorrectAlgorithm algo,
                                         bool printMO) : 
                                            m_pOverlapper(pOverlapper), 
                                            m_minOverlap(minOverlap),
                                            m_numOverlapRounds(numOverlapRounds),
                                            m_numKmerRounds(numKmerRounds),
                                            m_conflictCutoff(conflictCutoff),
                                            m_kmerLength(kmerLength),
                                            m_kmerThreshold(kmerThreshold),
                                            m_algorithm(algo),
                                            m_printOverlaps(printMO),
                                            m_depthFilter(10000)
{
}

//
ErrorCorrectProcess::~ErrorCorrectProcess()
{

}

//
ErrorCorrectResult ErrorCorrectProcess::process(const SequenceWorkItem& workItem)
{
    ErrorCorrectResult result = correct(workItem);
    if(!result.kmerQC && !result.overlapQC && m_printOverlaps)
        std::cout << workItem.read.id << " failed error correction QC\n";
    return result;
}
    
ErrorCorrectResult ErrorCorrectProcess::correct(const SequenceWorkItem& workItem)
{
    switch(m_algorithm)
    {
        case ECA_HYBRID:
        {
            ErrorCorrectResult result = kmerCorrection(workItem);
            if(!result.kmerQC)
                return overlapCorrection(workItem);
            else
                return result;
            break;
        }
        case ECA_KMER:
        {
            return kmerCorrection(workItem);
            break;
        }
        case ECA_OVERLAP:
        {
            return overlapCorrection(workItem);
            break;
        }
        default:
        {
            assert(false);
        }
    }
    ErrorCorrectResult result;
    return result;
}

ErrorCorrectResult ErrorCorrectProcess::overlapCorrection(const SequenceWorkItem& workItem)
{
    // Overlap based correction
    static const double p_error = 0.01f;
    bool done = false;
    int rounds = 0;
    
    ErrorCorrectResult result;
    SeqRecord currRead = workItem.read;
    std::string originalRead = workItem.read.seq.toString();

    while(!done)
    {
        // Compute the set of overlap blocks for the read
        m_blockList.clear();
        OverlapResult overlap_result = m_pOverlapper->overlapRead(currRead, m_minOverlap, &m_blockList);
        int sumOverlaps = 0;

        // Sum the spans of the overlap blocks to calculate the total number of overlaps this read has
        for(OverlapBlockList::iterator iter = m_blockList.begin(); iter != m_blockList.end(); ++iter)
        {
            assert(iter->ranges.interval[0].size() == iter->ranges.interval[1].size());
            sumOverlaps += iter->ranges.interval[0].size();
        }

        if(m_depthFilter > 0 && sumOverlaps > m_depthFilter)
        {
            result.num_prefix_overlaps = sumOverlaps;
            result.num_suffix_overlaps = sumOverlaps;
            result.correctSequence = currRead.seq;
            break;
        }

        // Convert the overlap block list into a multi-overlap 
        MultiOverlap mo = blockListToMultiOverlap(currRead, m_blockList);

        if(m_printOverlaps)
            mo.printMasked();

        result.num_prefix_overlaps = 0;
        result.num_suffix_overlaps = 0;
        mo.countOverlaps(result.num_prefix_overlaps, result.num_suffix_overlaps);

        // Perform conflict-aware consensus correction on the read
        result.correctSequence = mo.consensusConflict(p_error, m_conflictCutoff);

        ++rounds;
        if(rounds == m_numOverlapRounds || result.correctSequence == currRead.seq)
            done = true;
        else
            currRead.seq = result.correctSequence;
    }
    
    // Quality checks
    if(result.num_prefix_overlaps > 0 && result.num_suffix_overlaps > 0)
    {
        result.overlapQC = true;
    }
    else
    {
        result.overlapQC = false;
    }

    if(m_printOverlaps)
    {
        std::string corrected_seq = result.correctSequence.toString();
        std::cout << "OS:     " << originalRead << "\n";
        std::cout << "CS:     " << corrected_seq << "\n";
        std::cout << "DS:     " << getDiffString(originalRead, corrected_seq) << "\n";
        std::cout << "QS:     " << currRead.qual << "\n";
    	std::cout << "\n";
    }
    
    return result;
}

// Correct a read with a k-mer based corrector
ErrorCorrectResult ErrorCorrectProcess::kmerCorrection(const SequenceWorkItem& workItem)
{
    ErrorCorrectResult result;
    typedef std::map<std::string, int> KmerCountMap;
    KmerCountMap kmerCache;

    SeqRecord currRead = workItem.read;
    std::string readSequence = workItem.read.seq.toString();

#ifdef KMER_TESTING
    std::cout << "Kmer correcting read " << workItem.read.id << "\n";
#endif

    int n = readSequence.size();
    int nk = n - m_kmerLength + 1;
    
    // Are all kmers in the read well-represented?
    bool allSolid = false;
    bool done = false;
    int rounds = 0;
    int maxAttempts = m_numKmerRounds;

    // For each kmer, calculate the minimum phred score seen in the bases
    // of the kmer
    std::vector<int> minPhredVector(nk, 0);
    for(int i = 0; i < nk; ++i)
    {
        int end = i + m_kmerLength - 1;
        int minPhred = std::numeric_limits<int>::max();
        for(int j = i; j <= end; ++j)
        {
            int ps = workItem.read.getPhredScore(j);
            if(ps < minPhred)
                minPhred = ps;
        }
        minPhredVector[i] = minPhred;
    }

    while(!done && nk > 0)
    {
        // Compute the kmer counts across the read
        // and determine the positions in the read that are not covered by any solid kmers
        // These are the candidate incorrect bases
        std::vector<int> countVector(nk, 0);
        std::vector<int> solidVector(n, 0);

        for(int i = 0; i < nk; ++i)
        {
            std::string kmer = readSequence.substr(i, m_kmerLength);

            // First check if this kmer is in the cache
            // If its not, find its count from the fm-index and cache it
            int count = 0;
            KmerCountMap::iterator iter = kmerCache.find(kmer);

            if(iter != kmerCache.end())
            {
                count = iter->second;
            }
            else
            {
                count = BWTAlgorithms::countSequenceOccurrences(kmer, m_pOverlapper->getBWT(), m_pOverlapper->getRBWT());
                kmerCache.insert(std::make_pair(kmer, count));
            }

            // Get the phred score for the last base of the kmer
            int phred = minPhredVector[i];
            countVector[i] = count;
//            std::cout << i << "\t" << phred << "\t" << count << "\n";

            // Determine whether the base is solid or not based on phred scores
            int threshold = CorrectionThresholds::minSupportLowQuality;
            if(phred >= CorrectionThresholds::highQualityCutoff)
                threshold = CorrectionThresholds::minSupportHighQuality;
            if(count >= threshold)
            {
                for(int j = i; j < i + m_kmerLength; ++j)
                    solidVector[j] = 1;
            }
        }

        allSolid = true;
        for(int i = 0; i < n; ++i)
        {
#ifdef KMER_TESTING
            std::cout << "Position[" << i << "] = " << solidVector[i] << "\n";
#endif
            if(solidVector[i] != 1)
                allSolid = false;
        }
        
#ifdef KMER_TESTING  
        std::cout << "Read " << workItem.read.id << (allSolid ? " is solid\n" : " has potential errors\n");
#endif

        // Stop if all kmers are well represented or we have exceeded the number of correction rounds
        if(allSolid || rounds++ > maxAttempts)
            break;

        // Attempt to correct the leftmost potentially incorrect base
        bool corrected = false;
        for(int i = 0; i < n; ++i)
        {
            if(solidVector[i] != 1)
            {
                // Attempt to correct the base using the leftmost covering kmer
                int phred = workItem.read.getPhredScore(i);
                int threshold = CorrectionThresholds::minSupportLowQuality;
                if(phred >= CorrectionThresholds::highQualityCutoff)
                    threshold = CorrectionThresholds::minSupportHighQuality;

                int left_k_idx = (i + 1 >= m_kmerLength ? i + 1 - m_kmerLength : 0);
                corrected = attemptKmerCorrection(i, left_k_idx, std::max(countVector[left_k_idx], threshold), readSequence);
                if(corrected)
                    break;

                // base was not corrected, try using the rightmost covering kmer
                size_t right_k_idx = std::min(i, n - m_kmerLength);
                corrected = attemptKmerCorrection(i, right_k_idx, std::max(countVector[right_k_idx], threshold), readSequence);
                if(corrected)
                    break;
            }
        }

        // If no base in the read was corrected, stop the correction process
        if(!corrected)
        {
            assert(!allSolid);
            done = true;
        }
    }

    if(allSolid)
    {
        result.correctSequence = readSequence;
        result.kmerQC = true;
    }
    else
    {
        result.correctSequence = workItem.read.seq.toString();
        result.kmerQC = false;
    }
    return result;
}

// Attempt to correct the base at position idx in readSequence. Returns true if a correction was made
// The correction is made only if the count of the corrected kmer is at least minCount
bool ErrorCorrectProcess::attemptKmerCorrection(size_t i, size_t k_idx, size_t minCount, std::string& readSequence)
{
    assert(i >= k_idx && i < k_idx + m_kmerLength);
    size_t base_idx = i - k_idx;
    char originalBase = readSequence[i];
    std::string kmer = readSequence.substr(k_idx, m_kmerLength);
    size_t bestCount = 0;
    char bestBase = '$';

#if KMER_TESTING
    std::cout << "i: " << i << " k-idx: " << k_idx << " " << kmer << " " << reverseComplement(kmer) << "\n";
#endif

    for(int j = 0; j < DNA_ALPHABET::size; ++j)
    {
        char currBase = ALPHABET[j];
        if(currBase == originalBase)
            continue;
        kmer[base_idx] = currBase;
        size_t count = BWTAlgorithms::countSequenceOccurrences(kmer, m_pOverlapper->getBWT(), m_pOverlapper->getRBWT());

#if KMER_TESTING
        printf("%c %zu\n", currBase, count);
#endif
        if(count > bestCount && count >= minCount)
        {
            // Multiple corrections exist, do not correct
            if(bestBase != '$')
                return false;

            bestCount = count;
            bestBase = currBase;
        }
    }

    if(bestCount >= minCount)
    {
        assert(bestBase != '$');
        readSequence[i] = bestBase;
        return true;
    }
    return false;
}


//
//
//
ErrorCorrectPostProcess::ErrorCorrectPostProcess(std::ostream* pCorrectedWriter,
                                                 std::ostream* pDiscardWriter,
                                                 bool bCollectMetrics) : 
                                                      m_pCorrectedWriter(pCorrectedWriter),
                                                      m_pDiscardWriter(pDiscardWriter),
                                                      m_bCollectMetrics(bCollectMetrics),
                                                      m_totalBases(0), m_totalErrors(0),
                                                      m_readsKept(0), m_readsDiscarded(0),
                                                      m_kmerQCPassed(0), m_overlapQCPassed(0),
                                                      m_qcFail(0)
{

}

//
ErrorCorrectPostProcess::~ErrorCorrectPostProcess()
{
    std::cout << "Reads passed kmer QC check: " << m_kmerQCPassed << "\n";
    std::cout << "Reads passed overlap QC check: " << m_overlapQCPassed << "\n";
    std::cout << "Reads failed QC: " << m_qcFail << "\n";
}

//
void ErrorCorrectPostProcess::writeMetrics(std::ostream* pWriter)
{
    m_positionMetrics.write(pWriter, "Bases corrected by position\n", "pos");
    m_originalBaseMetrics.write(pWriter, "\nOriginal base that was corrected\n", "base");
    m_precedingSeqMetrics.write(pWriter, "\nkmer preceding the corrected base\n", "kmer");
    m_qualityMetrics.write(pWriter, "\nBases corrected by quality value\n\n", "quality");
        
    std::cout << "ErrorCorrect -- Corrected " << m_totalErrors << " out of " << m_totalBases <<
                 " bases (" << (double)m_totalErrors / m_totalBases << ")\n";
    std::cout << "Kept " << m_readsKept << " reads. Discarded " << m_readsDiscarded <<
                 " reads (" << (double)m_readsDiscarded / (m_readsKept + m_readsDiscarded)<< ")\n";
}

//
void ErrorCorrectPostProcess::process(const SequenceWorkItem& item, const ErrorCorrectResult& result)
{
    
    // Determine if the read should be discarded
    bool readQCPass = true;
    if(result.kmerQC)
    {
        m_kmerQCPassed += 1;
    }
    else if(result.overlapQC)
    {
        m_overlapQCPassed += 1;
    }
    else
    {
        readQCPass = false; 
        m_qcFail += 1;
    }

    // Collect metrics for the reads that were actually corrected
    if(m_bCollectMetrics && readQCPass)
    {
        collectMetrics(item.read.seq.toString(), 
                       result.correctSequence.toString(), 
                       item.read.qual);
    }

    SeqRecord record = item.read;
    record.seq = result.correctSequence;

    if(readQCPass || m_pDiscardWriter == NULL)
    {
        record.write(*m_pCorrectedWriter);
        ++m_readsKept;
    }
    else
    {
        record.write(*m_pDiscardWriter);
        ++m_readsDiscarded;
    }
}

void ErrorCorrectPostProcess::collectMetrics(const std::string& originalSeq,
                                             const std::string& correctedSeq,
                                             const std::string& qualityStr)
{
    size_t precedingLen = 2;
    for(size_t i = 0; i < originalSeq.length(); ++i)
    {
        char qc = !qualityStr.empty() ? qualityStr[i] : '\0';
        char ob = originalSeq[i];

        ++m_totalBases;
        
        m_positionMetrics.incrementSample(i);

        if(!qualityStr.empty())
            m_qualityMetrics.incrementSample(qc);

        m_originalBaseMetrics.incrementSample(ob);

        std::string precedingMer;
        if(i > precedingLen)
        {
            precedingMer = originalSeq.substr(i - precedingLen, precedingLen);
            m_precedingSeqMetrics.incrementSample(precedingMer);
        }

        if(originalSeq[i] != correctedSeq[i])
        {
            m_positionMetrics.incrementError(i);
            if(!qualityStr.empty())
                m_qualityMetrics.incrementError(qc);
            m_originalBaseMetrics.incrementError(ob);

            if(!precedingMer.empty())
            {
                m_precedingSeqMetrics.incrementError(precedingMer);
            }
            ++m_totalErrors;
        }
    }
}
