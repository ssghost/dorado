set dorado_bin=%1
set model=dna_r9.4.1_e8_hac@v3.3
set batch=384

echo dorado basecaller test stage
%dorado_bin% download --model %model%
%dorado_bin% basecaller %model% tests/data/pod5 -b %batch% --emit-fastq > ref.fq
%dorado_bin% basecaller %model% tests/data/pod5 -b %batch% --modified-bases 5mCG --emit-moves --reference ref.fq --emit-sam > calls.sam

echo dorado aligner test stage
%dorado_bin% aligner ref.fq calls.sam > aligned-calls.bam
