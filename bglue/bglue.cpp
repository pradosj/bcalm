/* remaining issue: 
 * - inconsistency of results between machines (laptop lifl vs cyberstar231) */
#include <gatb/gatb_core.hpp>
#include "unionFind.hpp"
#include "glue.hpp"
#include <atomic>
#include "../thirdparty/ThreadPool.h"

class bglue : public Tool
{
public:
    bglue() : Tool ("bglue"){
	getParser()->push_back (new OptionOneParam ("-k", "kmer size",  false,"31"));
	getParser()->push_back (new OptionOneParam ("-in", "input file",  true)); // necessary for repartitor
	getParser()->push_back (new OptionOneParam ("-out", "output file",  false, "out.fa"));
	getParser()->push_front (new OptionNoParam  ("--only-uf",   "(for debugging only) stop after UF construction", false));
	getParser()->push_front (new OptionNoParam  ("--uf-stats",   "display UF statistics", false));
    };

    // Actual job done by the tool is here
    void execute ();
};

using namespace std;

const size_t SPAN = KMER_SPAN(1); // TODO: adapt span using Minia's technique
typedef Kmer<SPAN>::Type  Type;
typedef Kmer<SPAN>::Count Count;
typedef Kmer<SPAN>::ModelCanonical ModelCanon;
typedef Kmer<SPAN>::ModelMinimizer <ModelCanon> Model;
size_t kmerSize=31;
size_t minSize=8;

   // a hash wrapper for hashing kmers in Model form
    template <typename ModelType>
    class Hasher_T 
    {
       public:
        ModelType model;
        
        Hasher_T(ModelType &model) : model(model) {};

        uint64_t operator ()  (const typename ModelType::Kmer& key, uint64_t seed = 0) const  {
                return model.getHash(key.value());
                }
    };

unsigned long memory_usage(string message="")
{
    // using Progress.cpp of gatb-core
    u_int64_t mem = System::info().getMemorySelfUsed() / 1024;
    u_int64_t memMaxProcess = System::info().getMemorySelfMaxUsed() / 1024;
    char tmp[128];
    snprintf (tmp, sizeof(tmp), "   memory [current, maximum (maxRSS)]: [%4lu, %4lu] MB ",
            mem, memMaxProcess);
    std::cout << message << " " << tmp << std::endl;
    return mem;
}

extern char revcomp (char s); // glue.cpp

string rc(string &s) {
	string rc;
	for (signed int i = s.length() - 1; i >= 0; i--) {rc += revcomp(s[i]);}
	return rc;
}


struct markedSeq
{
    string seq;
    bool lmark, rmark;
    string ks, ke; // [start,end] kmers of seq, in canonical form (redundant information with seq, but helpful)

    markedSeq(string seq, bool lmark, bool rmark, string ks, string ke) : seq(seq), lmark(lmark), rmark(rmark), ks(ks), ke(ke) {};

    void revcomp()
    {
        seq = rc(seq);
        std::swap(lmark, rmark);
        std::swap(ks, ke);
    }
};

vector<vector<markedSeq> > determine_order_sequences(vector<markedSeq> sequences)
{
    bool debug = false ;
    unordered_map<string, set<int> > kmerIndex;
    set<int> usedSeq;
    vector<vector<markedSeq>> res;
    unsigned int nb_chained = 0;

    // index kmers to their seq
    for (unsigned int i = 0; i < sequences.size(); i++)
    {
        kmerIndex[sequences[i].ks].insert(i);
        kmerIndex[sequences[i].ke].insert(i);
    }

    for (unsigned int i = 0; i < sequences.size(); i++)
    {
        markedSeq current = sequences[i];
        if (usedSeq.find(i) != usedSeq.end())
                continue; // this sequence has already been glued

        if (current.lmark & current.rmark)
            continue; // not the extremity of a chain

        if (current.lmark)
            current.revcomp(); // reverse so that lmark is false

        assert(current.lmark == false);

        vector<markedSeq> chain;
        chain.push_back(current);
    
        bool rmark = current.rmark;
        int current_index = i;
        int start_index = i;
        usedSeq.insert(i);

        while (rmark)
        {
            if (debug)
                std::cout << "current ke " << current.ke << " index " << current_index << " markings: " << current.lmark << current.rmark <<std::endl;

            set<int> candidateSuccessors = kmerIndex[current.ke];
            assert(candidateSuccessors.find(current_index) != candidateSuccessors.end());
            candidateSuccessors.erase(current_index);
            assert(candidateSuccessors.size() == 1); // normally there is exactly one sequence to glue with

            int successor_index = *candidateSuccessors.begin(); // pop()
            assert(successor_index != current_index);
            markedSeq successor = sequences[successor_index]; 

            if (successor.ks != current.ke || (!successor.lmark))
                successor.revcomp();
            
            if (debug)
                std::cout << "successor " << successor_index << " successor ks ke "  << successor.ks << " "<< successor.ke << " markings: " << successor.lmark << successor.rmark << std::endl;
            
            assert(successor.lmark);
            assert(successor.ks == current.ke);

            if (successor.ks == successor.ke)
            {
                if (debug)
                    std::cout << "successor seq loops: " << successor.seq << std::endl;
                assert(successor.seq.size() == kmerSize);
                if (successor.lmark == false)
                    assert(successor.rmark == true); 
                else
                    assert(successor.rmark == false);
                // it's the only possible cases I can think of
                
                // there is actually nothing to be done now, it's an extremity, so it will end.
                // on a side note, it's pointless to save this kmer in bcalm.
            }
            

            current = successor;
            chain.push_back(current);
            current_index = successor_index;
            rmark = current.rmark;
            assert((usedSeq.find(current_index) == usedSeq.end()));
            usedSeq.insert(current_index);
        }

        res.push_back(chain);
        nb_chained += chain.size();
    }
    assert(sequences.size() == nb_chained); // make sure we've scheduled to glue all sequences in this partition
    return res;
}

/* straightforward glueing of a chain
 * sequences should be ordered and in the right orientation
 * so, it' just a matter of chopping of the first kmer
 */
string glue_sequences(vector<markedSeq> chain)
{
    string res;
    string previous_kmer = "";
    unsigned int k = kmerSize;
    bool last_rmark = false;

    for (auto it = chain.begin(); it != chain.end(); it++)
    {
        string seq = it->seq;

        if (previous_kmer.size() == 0) // it's the first element in a chain
        {
            assert(it->lmark == false);
            res += seq;
        }
        else
        {
            assert(seq.substr(0, k).compare(previous_kmer) == 0);
            res += seq.substr(k);
        }
        
        previous_kmer = seq.substr(seq.size() - k);
        assert(previous_kmer.size() == k);
        last_rmark = it->rmark;
    }
    assert(last_rmark == false);
    if (last_rmark) { cout<<"bad gluing, missed an element" << endl; exit(1); } // in case assert()'s are disabled

    return res;
}

void output(string seq, BankFasta &out, string comment = "")
{
    Sequence s (Data::ASCII);
    s.getData().setRef ((char*)seq.c_str(), seq.size());
    s._comment = comment;   
    out.insert(s);
    out.flush(); // TODO: see if we can do without this flush; would require a dedicated thread for writing to file
}


/* main */
void bglue::execute (){

    int nb_threads = getInput()->getInt("-nb-cores");
    std::cout << "Nb threads: " << nb_threads <<endl;
    kmerSize=getInput()->getInt("-k");
    size_t k = kmerSize;
    string inputFile(getInput()->getStr("-in")); // necessary for repartitor

    string h5_prefix = inputFile.substr(0,inputFile.size()-2);
    BankFasta in (h5_prefix + "glue");


    Storage* storage = StorageFactory(STORAGE_HDF5).load ( inputFile.c_str() );

    LOCAL (storage);
    /** We get the dsk and minimizers hash group in the storage object. */
    Group& dskGroup = storage->getGroup("dsk");
    Group& minimizersGroup = storage->getGroup("minimizers");

    typedef Kmer<SPAN>::Count Count;
    Partition<Count>& partition = dskGroup.getPartition<Count> ("solid");
    size_t nbPartitions = partition.size();
    cout << "DSK created " << nbPartitions << " partitions" << endl;

    /** We retrieve the minimizers distribution from the solid kmers storage. */
    Repartitor repart;
    repart.load (minimizersGroup);

    u_int64_t rg = ((u_int64_t)1 << (2*minSize));

    /* Retrieve frequency of minimizers;
     * actually only used in minimizerMin and minimizerMax */
    uint32_t *freq_order = NULL;

    int minimizer_type = 1; // typical bcalm usage.
    if (minimizer_type == 1) 
    {
        freq_order = new uint32_t[rg];
        Storage::istream is (minimizersGroup, "minimFrequency");
        is.read ((char*)freq_order, sizeof(uint32_t) * rg);
    }

#if 0  // all those models are for creating UF with k-1 mers or minimizers, we don't do that anymore. legacy/debugging code, that can be removed later.
    Model model(kmerSize, minSize, Kmer<SPAN>::ComparatorMinimizerFrequencyOrLex(), freq_order);
    Model modelK1(kmerSize-1, minSize,  Kmer<SPAN>::ComparatorMinimizerFrequencyOrLex(), freq_order);
    Model modelK2(kmerSize-2, minSize,  Kmer<SPAN>::ComparatorMinimizerFrequencyOrLex(), freq_order);
    Model modelM(minSize, minSize,  Kmer<SPAN>::ComparatorMinimizerFrequencyOrLex(), freq_order);
#endif

    // create a hasher for UF
    ModelCanon modelCanon(kmerSize); // i'm a bit lost with those models.. I think GATB could be made more simple here.
    Hasher_T<ModelCanon> hasher(modelCanon);

    // create a UF data structure
#if 0
    unionFind<unsigned int> ufmin;
    unionFind<std::string> ufprefixes; 
    unsigned int prefix_length = 10;
    unionFind<std::string> ufkmerstr; 
#endif
    // those were toy one, here is the real one:

    typedef uint32_t partition_t; 
    unionFind<partition_t> ufkmers(0); 
    // instead of UF of kmers, we do a union find of hashes of kmers. less memory. will have collisions, but that's okay i think. let's see.
    // actually, in the current implementation, partition_t is not used, but values are indeed hardcoded in 32 bits (the UF implementation uses a 64 bits hash table for internal stuff)
    
    // We create an iterator over this bank.
    BankFasta::Iterator it (in);

    // We loop over sequences.
    /*for (it.first(); !it.isDone(); it.next())
    {
        string seq = it->toString();*/
    auto createUF = [k, &modelCanon /* crashes if copied!*/, \
        &ufkmers, &hasher](const Sequence& sequence)
    {
        string seq = sequence.toString();

        if (seq.size() < k) 
        {
            std::cout << "unexpectedly small sequence found ("<<seq.size()<<"). did you set k correctly?" <<std::endl; exit(1);
        }
        string comment = sequence.getComment();
        bool lmark = comment[0] == '1';
        bool rmark = comment[1] == '1';

        if (!(lmark & rmark)) // if either mark is 0, it's an unitig extremity, so nothing to glue here
            return;

        string kmerBegin = seq.substr(0, k );
        string kmerEnd = seq.substr(seq.size() - k , k );

        // UF of canonical kmers in ModelCanon form, then hashed 
        ModelCanon::Kmer kmmerBegin = modelCanon.codeSeed(kmerBegin.c_str(), Data::ASCII);
        ModelCanon::Kmer kmmerEnd = modelCanon.codeSeed(kmerEnd.c_str(), Data::ASCII);
        ufkmers.union_(hasher(kmmerBegin), hasher(kmmerEnd));

#if 0 

        Model::Kmer kmmerBegin = model.codeSeed(kmerBegin.c_str(), Data::ASCII);
        Model::Kmer kmmerEnd = model.codeSeed(kmerEnd.c_str(), Data::ASCII);
 
        // UF of canonical kmers in string form, not hashed
        string canonicalKmerBegin = modelK1.toString(kmmerBegin.value());
        string canonicalKmerEnd = modelK1.toString(kmmerEnd.value());
        ufkmerstr.union_(canonicalKmerBegin, canonicalKmerEnd);

        // UF of minimizers of kmers
        size_t leftMin(modelK1.getMinimizerValue(kmmerBegin.value()));
        size_t rightMin(modelK1.getMinimizerValue(kmmerEnd.value()));
        ufmin.union_(leftMin, rightMin);

        // UF of prefix of kmers in string form
        string prefixCanonicalKmerBegin = canonicalKmerBegin.substr(0, prefix_length);
        string prefixCanonicalKmerEnd = canonicalKmerEnd.substr(0, prefix_length);
        ufprefixes.union_(prefixCanonicalKmerBegin, prefixCanonicalKmerEnd);
#endif    
        

    };

    //setDispatcher (new SerialDispatcher()); // force single thread
    setDispatcher (  new Dispatcher (nb_threads) );
    getDispatcher()->iterate (it, createUF);

#if 0
    ufmin.printStats("uf minimizers");

    ufprefixes.printStats("uf " + to_string(prefix_length) + "-prefixes of kmers");
    
    ufkmerstr.printStats("uf kmers, std::string");
#endif
    

    unsigned long memUF = memory_usage("UF constructed");

    if (getParser()->saw("--uf-stats")) // for debugging
    {
        ufkmers.printStats("uf kmers");
        unsigned long memUFpostStats = memory_usage("after computing UF stats");
    }

    if (getParser()->saw("--only-uf")) // for debugging
        return;

    // setup output file
    string output_prefix = getInput()->getStr("-out");
    BankFasta out (output_prefix);

    auto get_partition = [&modelCanon, &ufkmers, &hasher]
        (string &kmerBegin, string &kmerEnd, 
         bool lmark, bool rmark,
         ModelCanon::Kmer &kmmerBegin, ModelCanon::Kmer &kmmerEnd,  // those will be populated based on lmark and rmark
         bool &found_partition)
        {
            found_partition = false;
            partition_t partition = 0;

            if (lmark)
            {
                kmmerBegin = modelCanon.codeSeed(kmerBegin.c_str(), Data::ASCII);
                found_partition = true;
                partition = ufkmers.find(hasher(kmmerBegin));
            }

            if (rmark)
            {
                kmmerEnd = modelCanon.codeSeed(kmerEnd.c_str(), Data::ASCII);
                if (found_partition) // just do a small check
                {   
                    if (ufkmers.find(hasher(kmmerEnd)) != partition)
                    { std::cout << "bad UF! left kmer has partition " << partition << " but right kmer has partition " << ufkmers.find(hasher(kmmerEnd)) << std::endl; exit(1); }
                }
                else
                {
                    partition = ufkmers.find(hasher(kmmerEnd));
                    found_partition = true;
                }
            }

            return partition;
        };

    int nbGluePartitions = 200;
    std::mutex *gluePartitionsLock = new std::mutex[nbGluePartitions];
    std::mutex outLock; // for the main output file
    std::vector<BankFasta*> gluePartitions(nbGluePartitions);
    std::string gluePartition_prefix = output_prefix + ".gluePartition.";
    for (int i = 0; i < nbGluePartitions; i++)
        gluePartitions[i] = new BankFasta(gluePartition_prefix + std::to_string(i));

    // partition the glue into many files, à la dsk
    auto partitionGlue = [k, &modelCanon /* crashes if copied!*/, \
        &ufkmers, &hasher, &get_partition, &gluePartitions, &gluePartitionsLock,
        nbGluePartitions, &out, &outLock]
            (const Sequence& sequence)
    {
        string seq = sequence.toString();

        string comment = sequence.getComment();
        bool lmark = comment[0] == '1';
        bool rmark = comment[1] == '1';

        string kmerBegin = seq.substr(0, k );
        string kmerEnd = seq.substr(seq.size() - k , k );

        // make canonical kmer
        ModelCanon::Kmer kmmerBegin;
        ModelCanon::Kmer kmmerEnd;

        bool found_partition = false;

        partition_t partition = get_partition(kmerBegin, kmerEnd, lmark, rmark, kmmerBegin, kmmerEnd, found_partition);

        // compute kmer extremities if we have not already
        if (!lmark)
            kmmerBegin = modelCanon.codeSeed(kmerBegin.c_str(), Data::ASCII);
        if (!rmark)
            kmmerEnd = modelCanon.codeSeed(kmerEnd.c_str(), Data::ASCII);

        if (!found_partition) // this one doesn't need to be glued 
        {
            outLock.lock();
            output(seq, out); // maybe could optimize writing by using queues
            outLock.unlock();
            return;
        }

        int index = partition % nbGluePartitions;
        gluePartitionsLock[index].lock();
        //stringstream ss1; // to save partition later
        //ss1 << blabla;
        output(seq, *gluePartitions[index], comment);
        gluePartitionsLock[index].unlock();
    };

    cout << "Disk partitioning of glue " << endl;

    setDispatcher (  new Dispatcher (getInput()->getInt(STR_NB_CORES)) );
    getDispatcher()->iterate (it, partitionGlue);

    cout << "Glueing partitions" << endl;
   
    // glue all partitions using a thread pool     
    ThreadPool pool(nb_threads - 1);

    for (int partition = 0; partition < nbGluePartitions; partition++)
    {
        auto glue_partition = [&modelCanon, &ufkmers, &hasher, partition, &gluePartition_prefix,
        &get_partition, &out, &outLock]()
        {
            int k = kmerSize;

            string partitionFile = gluePartition_prefix + std::to_string(partition);
            BankFasta partitionBank (partitionFile);

            outLock.lock(); // should use a printlock..
            std::cout << "Loading partition " << partition << " (size: " << System::file().getSize(partitionFile)/1024/1024 << " MB)" << std::endl;
            outLock.unlock();

            BankFasta::Iterator it (partitionBank);

            unordered_map<int,vector<markedSeq>> msInPart;

            for (it.first(); !it.isDone(); it.next()) 
            {
                string seq = it->toString();

                string kmerBegin = seq.substr(0, k );
                string kmerEnd = seq.substr(seq.size() - k , k );

                partition_t partition = 0;
                bool found_partition = false;

                string comment = it->getComment();
                bool lmark = comment[0] == '1';
                bool rmark = comment[1] == '1';

                // TODO get partition id from sequence header (save it previously)

                // make canonical kmer
                ModelCanon::Kmer kmmerBegin;
                ModelCanon::Kmer kmmerEnd;

                partition = get_partition(kmerBegin, kmerEnd, lmark, rmark, kmmerBegin, kmmerEnd, found_partition);

                // compute kmer extremities if we have not already
                if (!lmark)
                    kmmerBegin = modelCanon.codeSeed(kmerBegin.c_str(), Data::ASCII);
                if (!rmark)
                    kmmerEnd = modelCanon.codeSeed(kmerEnd.c_str(), Data::ASCII);

                string ks = modelCanon.toString(kmmerBegin.value());
                string ke = modelCanon.toString(kmmerEnd  .value());
                markedSeq ms(seq, lmark, rmark, ks, ke);

                msInPart[partition].push_back(ms);
            }


            // now iterates all sequences in a partition to glue them in clever order (avoid intermediate gluing)
            for (auto it = msInPart.begin(); it != msInPart.end(); it++)
            {
                //std::cout << "1.processing partition " << it->first << std::endl;
                vector<vector<markedSeq>> ordered_sequences = determine_order_sequences(it->second);
                //std::cout << "2.processing partition " << it->first << " nb ordered sequences: " << ordered_sequences.size() << std::endl;

                for (auto itO = ordered_sequences.begin(); itO != ordered_sequences.end(); itO++)
                {
                    string seq = glue_sequences(*itO);

                    outLock.lock();
                    output(seq, out); // maybe could optimize writing by using queues
                    outLock.unlock();
                }
            }

            partitionBank.finalize();

            System::file().remove (partitionFile);

        };
    
        pool.enqueue(glue_partition);
    }

    pool.join();

    memory_usage("end");

   
//#define ORIGINAL_GLUE
#ifdef ORIGINAL_GLUE
    // We loop again over sequences
    // but this time we glue!
    int nb_glues = 1;
    BankFasta out (getInput()->getStr("-out") + ".original_glue_version");
    GlueCommander glue_commander(kmerSize, &out, nb_glues, &model);
    for (it.first(); !it.isDone(); it.next())
    {
        string seq = it->toString();
        string comment = it->getComment();
        bool lmark = comment[0] == '1';
        bool rmark = comment[1] == '1';
        GlueEntry e(seq, lmark, rmark, kmerSize);
        glue_commander.insert(e);
    }

    glue_commander.stop();
    cout << "Final glue:\n";
    glue_commander.dump();
    cout << "*****\n";
    glue_commander.printMemStats();
#endif

}

int main (int argc, char* argv[])
{
    try
    {
        bglue().run (argc, argv);
    }
    catch (Exception& e)
    {
        std::cout << "EXCEPTION: " << e.getMessage() << std::endl;
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}
