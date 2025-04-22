#include "test.h"

#include <iostream>
#include <unordered_map>

void print_test(){
    std::cout << "hanbo is tesing print !" << std::endl;

    std::unordered_map<int, int> mm;
    mm[0] = 1;
    std::cout << "mm[0] == " << mm[0] << std::endl;

    return;
}