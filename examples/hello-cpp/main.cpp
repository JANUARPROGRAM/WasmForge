#include <iostream>
#include <vector>
#include "util/sum.hpp"

int main() {
  std::vector<int> v{1, 2, 3, 4};
  std::cout << "sum = " << sum(v) << "\n";
  return 0;
}
