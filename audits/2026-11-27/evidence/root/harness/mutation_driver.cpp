#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <random>
#include <string>
#include <vector>
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t*, std::size_t);
int main(int argc,char** argv) {
    if(argc<4) return 2;
    const auto count=std::stoull(argv[1]);
    const std::filesystem::path last=argv[2];
    std::vector<std::vector<std::uint8_t>> seeds;
    for(int i=3;i<argc;++i) {
        std::ifstream in(argv[i],std::ios::binary);
        std::vector<std::uint8_t> bytes(std::istreambuf_iterator<char>{in},{});
        if(bytes.size()<=1024*1024) seeds.push_back(std::move(bytes));
    }
    if(seeds.empty()) seeds.push_back({0});
    std::mt19937_64 rng{20261127};
    for(std::size_t i=0;i<count;++i) {
        auto bytes=seeds[i%seeds.size()];
        if(i>=seeds.size()) {
            switch(i%5) {
            case 0: if(!bytes.empty()) bytes.resize(rng()%(bytes.size()+1)); break;
            case 1: for(unsigned j=0;j<1+rng()%8 && !bytes.empty();++j) bytes[rng()%bytes.size()]=static_cast<std::uint8_t>(rng()); break;
            case 2: bytes.insert(bytes.end(),1+rng()%128,static_cast<std::uint8_t>(rng())); break;
            case 3: if(bytes.size()>8) {const auto at=rng()%(bytes.size()-7); std::fill_n(bytes.begin()+static_cast<std::ptrdiff_t>(at),8,0xff);} break;
            default: bytes.resize(rng()%1024); for(auto& b:bytes)b=static_cast<std::uint8_t>(rng()); break;
            }
        }
        {std::ofstream out(last,std::ios::binary); out.write(reinterpret_cast<const char*>(bytes.data()),static_cast<std::streamsize>(bytes.size()));}
        if(i%100==0){std::printf("iteration=%zu bytes=%zu seeds=%zu\n",i,bytes.size(),seeds.size());std::fflush(stdout);}
        LLVMFuzzerTestOneInput(bytes.data(),bytes.size());
    }
    std::printf("completed=%llu seed_count=%zu seed=20261127\n",static_cast<unsigned long long>(count),seeds.size());
}
