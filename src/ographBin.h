#ifndef OGRAPHBIN
#define OGRAPHBIN

#include <iostream>
#include <fstream>
#include <vector>
#include <array>
#include <string>
#include <cstdlib>
#include "binSeq.h"


using namespace std;


class graph4{
	public:
		uint k,indiceUnitigs,nbElement,minimizer,minsize;
		binSeq* unitigs;
		vector<overlapIndice> left;
		vector<overlapIndice> right;
		void addvertex(string& str);
        void addtuple(tuple<binSeq,uint,uint>& tuple);
		void addleftmin(unsigned int mini);
		void addrightmin(unsigned int mini);
		void debruijn();
        void debruijn2();
        void compaction2(uint iL, uint iR);
		void compress();
        kmer rcb(kmer min);
		void compaction(uint iR, uint iL);
		uint size();
        bool output(uint i);
        void clear();

		graph4(uint ka, uint min,uint size, uint nb){
            indiceUnitigs=0;
			minsize=size;
			k=ka;
			minimizer=min;
            nbElement=nb;
            unitigs=new binSeq [nbElement];
            left.reserve(nbElement);
    	    right.reserve(nbElement);
		}
};


void compareUnitigs(const string& fileFa,const string& fileDot);
void compareKmers(const string& fileFa,const string& fileDot);



#endif