/// @author    Johannes de Fine Licht (definelicht@inf.ethz.ch)
/// @date      June 2017
/// @copyright This software is copyrighted under the BSD 3-Clause License.

#include <algorithm>
#include <cmath>
#include <iostream>
#include <random>
#include <vector>
#include "MatrixMultiplication.h"
#include "Utility.h"
#include "hlslib/SDAccel.h"
#include "hlslib/Utility.h"

void PrintUsage() {
  std::cout
      << "Usage: ./RunHardware.exe <mode [hw/hw_emu]> [<verify [on/off]>]\n"
      << std::flush;
}

int main(int argc, char **argv) {

  std::default_random_engine rng(kSeed);
  typename std::conditional<std::is_integral<Data_t>::value,
                            std::uniform_int_distribution<unsigned long>,
                            std::uniform_real_distribution<double>>::type
      dist(1, 10);

  bool emulation = false;
  bool verify = true;
  hlslib::UnsetEnvironmentVariable("XCL_EMULATION_MODE");
  std::string path = "MatrixMultiplication_hw.xclbin";
  if (argc > 3) {
    PrintUsage();
    return 1;
  }
  if (argc > 1) {
    const std::string emulation_arg(argv[1]);
    if (emulation_arg == "hw_emu") {
      emulation = true;
      hlslib::SetEnvironmentVariable("XCL_EMULATION_MODE", "hw_emu");
      path = "MatrixMultiplication_hw_emu.xclbin";
    } else if (emulation_arg != "hw") {
      PrintUsage();
      return 1;
    }
  }
  if (argc > 2) {
    const std::string verify_arg(argv[2]);
    if (verify_arg == "off") {
      verify = false;
    } else if (verify_arg != "on") {
      PrintUsage();
      return 1;
    }
  }

  std::vector<Data_t> a, b, cRef;
  std::vector<MemoryPack_t, hlslib::ocl::AlignedAllocator<MemoryPack_t, 4096>>
      aMem, bMem, cMem;
  std::cout << "Initializing host memory..." << std::flush;
  if (verify) {
    a = decltype(a)(kSizeN * kSizeK);
    std::for_each(a.begin(), a.end(),
                  [&dist, &rng](Data_t &in) { in = Data_t(dist(rng)); });
    b = decltype(b)(kSizeK * kSizeM);
    std::for_each(b.begin(), b.end(),
                  [&dist, &rng](Data_t &in) { in = Data_t(dist(rng)); });
    cRef = decltype(cRef)(kSizeN * kSizeM, 0);

    aMem = Pack(a);
    bMem = Pack(b);
    cMem = Pack(cRef);
  }
  std::cout << " Done.\n";

  try {
    std::cout << "Initializing OpenCL context...\n" << std::flush;
    hlslib::ocl::Context context;

    std::cout << "Programming device...\n" << std::flush;
    auto program = context.MakeProgram(path);

    std::cout << "Initializing device memory...\n" << std::flush;
    auto aDevice = context.MakeBuffer<MemoryPack_t, hlslib::ocl::Access::read>(
        hlslib::ocl::MemoryBank::bank0, kSizeN * kSizeK / kMemoryWidth);
    auto bDevice = context.MakeBuffer<MemoryPack_t, hlslib::ocl::Access::read>(
        hlslib::ocl::MemoryBank::bank1, kSizeK * kSizeM / kMemoryWidth);
    auto cDevice = context.MakeBuffer<MemoryPack_t, hlslib::ocl::Access::write>(
        hlslib::ocl::MemoryBank::bank1, kSizeN * kSizeM / kMemoryWidth);

    if (verify) {
      std::cout << "Copying memory to device...\n" << std::flush;
      aDevice.CopyFromHost(aMem.cbegin());
      bDevice.CopyFromHost(bMem.cbegin());
      cDevice.CopyFromHost(cMem.cbegin());
    }

    std::cout << "Creating kernel...\n" << std::flush;
    auto kernel = program.MakeKernel("MatrixMultiplicationKernel", aDevice,
                                     bDevice, cDevice);

    std::cout << "Executing kernel...\n" << std::flush;
    const auto elapsed = kernel.ExecuteTask();

    const auto perf = 1e-9 *
                      (2 * static_cast<float>(kSizeN) * kSizeK * kSizeM) /
                      elapsed.first;

    std::cout << "Kernel executed in " << elapsed.first
              << " seconds, corresponding to a performance of " << perf
              << " GOp/s.\n";

    if (verify) {
      std::cout << "Copying back result...\n" << std::flush;
      cDevice.CopyToHost(cMem.begin());
    }

  } catch (std::runtime_error const &err) {
    std::cerr << "Execution failed with error: \"" << err.what() << "\"."
              << std::endl;
    return 1;
  }

  // Run reference implementation
  if (verify) {
    std::cout << "Running reference implementation...\n" << std::flush;
    ReferenceImplementation(a.data(), b.data(), cRef.data());

    std::cout << "Verifying result...\n" << std::flush;
    // Convert to single element vector
    const auto cTest = Unpack(cMem);

    for (int i = 0; i < kSizeN; ++i) {
      for (int j = 0; j < kSizeM; ++j) {
        const auto testVal = cTest[i * kSizeM + j];
        const auto refVal = cRef[i * kSizeM + j];
        const auto diff = std::abs(testVal - refVal);
        if (diff > static_cast<Data_t>(1e-3)) {
          std::cerr << "Mismatch at (" << i << ", " << j << "): " << testVal
                    << " vs. " << refVal << "\n";
          return 1;
        }
      }
    }
    std::cout << "Successfully verified." << std::endl;
  }

  return 0;
}
