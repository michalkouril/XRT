// OpenCL on macOS through XRT's ICD: enumerate, then run the hello kernel
// from verify.xclbin and read back "Hello World".
#define CL_TARGET_OPENCL_VERSION 120
#include <CL/cl.h>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <vector>
#define CHECK(x) do { cl_int e_ = (x); if (e_ != CL_SUCCESS) { std::printf("%s failed: %d\n", #x, e_); return 1; } } while (0)
int main(int argc, char** argv) {
  cl_uint np = 0;
  CHECK(clGetPlatformIDs(0, nullptr, &np));
  std::vector<cl_platform_id> ps(np);
  CHECK(clGetPlatformIDs(np, ps.data(), nullptr));
  std::printf("%u platform(s)\n", np);
  cl_platform_id xp = nullptr;
  for (auto p : ps) {
    char name[128] = {}, vendor[128] = {}, ver[128] = {};
    clGetPlatformInfo(p, CL_PLATFORM_NAME, sizeof(name), name, nullptr);
    clGetPlatformInfo(p, CL_PLATFORM_VENDOR, sizeof(vendor), vendor, nullptr);
    clGetPlatformInfo(p, CL_PLATFORM_VERSION, sizeof(ver), ver, nullptr);
    std::printf("  platform: %s / %s / %s\n", name, vendor, ver);
    if (std::strstr(vendor, "Xilinx")) xp = p;
  }
  if (!xp) { std::printf("no Xilinx platform\n"); return 1; }
  cl_uint nd = 0;
  CHECK(clGetDeviceIDs(xp, CL_DEVICE_TYPE_ACCELERATOR, 0, nullptr, &nd));
  std::vector<cl_device_id> ds(nd);
  CHECK(clGetDeviceIDs(xp, CL_DEVICE_TYPE_ACCELERATOR, nd, ds.data(), nullptr));
  char dname[256] = {};
  clGetDeviceInfo(ds[0], CL_DEVICE_NAME, sizeof(dname), dname, nullptr);
  std::printf("%u device(s): %s\n", nd, dname);
  if (argc < 2) return 0;

  std::ifstream f(argv[1], std::ios::binary);
  std::vector<unsigned char> bin((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  cl_int err = 0;
  cl_context ctx = clCreateContext(nullptr, 1, &ds[0], nullptr, nullptr, &err); CHECK(err);
  cl_command_queue q = clCreateCommandQueue(ctx, ds[0], 0, &err); CHECK(err);
  const unsigned char* bp = bin.data(); size_t bs = bin.size(); cl_int bst = 0;
  cl_program prog = clCreateProgramWithBinary(ctx, 1, &ds[0], &bs, &bp, &bst, &err); CHECK(err);
  CHECK(clBuildProgram(prog, 1, &ds[0], nullptr, nullptr, nullptr));
  cl_kernel k = clCreateKernel(prog, "hello", &err); CHECK(err);
  char zero[1024] = {};
  cl_mem buf = clCreateBuffer(ctx, CL_MEM_READ_WRITE | CL_MEM_COPY_HOST_PTR, sizeof(zero), zero, &err); CHECK(err);
  CHECK(clSetKernelArg(k, 0, sizeof(cl_mem), &buf));
  CHECK(clEnqueueTask(q, k, 0, nullptr, nullptr));
  char out[1024] = {};
  CHECK(clEnqueueReadBuffer(q, buf, CL_TRUE, 0, sizeof(out), out, 0, nullptr, nullptr));
  CHECK(clFinish(q));
  bool ok = std::strncmp(out, "Hello World\n", 12) == 0;
  std::printf("kernel output: \"%.*s\" -> %s\n", 11, out, ok ? "PASS" : "FAIL");
  clReleaseMemObject(buf); clReleaseKernel(k); clReleaseProgram(prog); clReleaseCommandQueue(q); clReleaseContext(ctx);
  return ok ? 0 : 1;
}
