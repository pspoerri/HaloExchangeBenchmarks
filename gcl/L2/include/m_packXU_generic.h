
/*
Copyright (c) 2012, MAURO BIANCO, UGO VARETTO, SWISS NATIONAL SUPERCOMPUTING CENTRE (CSCS)
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:
    * Redistributions of source code must retain the above copyright
      notice, this list of conditions and the following disclaimer.
    * Redistributions in binary form must reproduce the above copyright
      notice, this list of conditions and the following disclaimer in the
      documentation and/or other materials provided with the distribution.
    * Neither the name of the Swiss National Supercomputing Centre (CSCS) nor the
      names of its contributors may be used to endorse or promote products
      derived from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL MAURO BIANCO, UGO VARETTO, OR 
SWISS NATIONAL SUPERCOMPUTING CENTRE (CSCS), BE LIABLE FOR ANY
DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

This code have been developed with the collaboration of Peter Messmer
*/

#include <wrap_argument.h>

template <typename value_type>
__global__ void m_packXUKernel_generic(const value_type* __restrict__ d_data, 
                                       value_type** __restrict__ d_msgbufTab, 
                                       const wrap_argument d_msgsize,
                                       const GCL::array<GCL::halo_descriptor,3> halo/*_g*/, 
                                       int const ny, int const nz, 
                                       int const field_index){
 
   // per block shared buffer for storing destination buffers
   __shared__ value_type* msgbuf[27];
   //__shared__ GCL::halo_descriptor halo[3];

   int idx = blockIdx.x;
   int idy = blockIdx.y * blockDim.y + threadIdx.y;
   int idz = blockIdx.z * blockDim.z + threadIdx.z;

   // load msg buffer table into shmem. Only the first 9 threads 
   // need to do this
   if(threadIdx.x == 0 && threadIdx.y < 27 && threadIdx.z == 0) {
    msgbuf[threadIdx.y] =  d_msgbufTab[threadIdx.y]; 
   }

   // load the data from the contiguous source buffer
   value_type x;
   if((idy < ny) && (idz < nz)) {
     int mas=halo[0].minus();

     int ia = idx + halo[0].end() - mas + 1;;
     int ib = idy + halo[1].begin();
     int ic = idz + halo[2].begin();
     int isrc  = ia + ib * halo[0].total_length() + ic * halo[0].total_length() * halo[1].total_length();
     x =  d_data[isrc]; 
     //     printf("XU %e\n", x);
   }

   int ba = 2;
   int la = halo[0].minus(); 
     
   int bb = 1;
   int lb = halo[1].end() - halo[1].begin() + 1;

   int bc = 1;

   int b_ind = ba + 3*bb + 9*bc;

   int oa = idx;
   int ob = idy;
   int oc = idz;

   int idst = oa + ob * la + oc * la * lb + d_msgsize[b_ind];;

   // at this point we need to be sure that threads 0 - 8 have loaded the
   // message buffer table. 
   __syncthreads();
  
    // store the data in the correct message buffer
   if((idy < ny) && (idz < nz)) {
     //printf("XU %d %d %d -> %16.16e\n", idx, idy, idz, x);
     msgbuf[b_ind][idst] = x;
   }
}

template <typename array_t>
void m_packXU_generic(array_t const& fields, 
                      typename array_t::value_type::value_type** d_msgbufTab, 
                      int* d_msgsize)
{

#ifdef CUDAMSG
// just some timing stuff
  cudaEvent_t start, stop;
  cudaEventCreate(&start);
  cudaEventCreate(&stop);

  cudaEventRecord(start, 0);
#endif 

  const int niter = fields.size();

  // run the compression a few times, just to get a bit
  // more statistics
  for(int i=0; i < niter; i++){

    // threads per block. Should be at least one warp in x, could be wider in y
    const int ntx = 1;                 
    const int nty = 32;
    const int ntz = 8;
    dim3 threads(ntx, nty, ntz);

    // form the grid to cover the entire plane. Use 1 block per z-layer
    int nx = fields[i].halos[0].s_length(1);
    int ny = fields[i].halos[1].s_length(0);
    int nz = fields[i].halos[2].s_length(0);

    int nbx = (nx + ntx - 1) / ntx ;
    int nby = (ny + nty - 1) / nty ;
    int nbz = (nz + ntz - 1) / ntz ;
    dim3 blocks(nbx, nby, nbz); 

#ifdef CUDAMSG
    printf("PACK XU Launch grid (%d,%d,%d) with (%d,%d,%d) threads (full size: %d,%d,%d)\n", 
           nbx, nby, nbz, ntx, nty, ntz, nx, ny, nz); 
#endif 

    if (nbx!=0 && nby!=0 && nbz!=0) {
      // the actual kernel launch
        m_packXUKernel_generic<<<blocks, threads, 0, XU_stream>>>
        (fields[i].ptr, 
         reinterpret_cast<typename array_t::value_type::value_type**>(d_msgbufTab), 
         wrap_argument(d_msgsize+27*i), 
         *(reinterpret_cast<const GCL::array<GCL::halo_descriptor,3>*>(&fields[i])),
         ny, 
         nz, 
         0); 
#ifdef CUDAMSG
      int err = cudaGetLastError();
      if(err != cudaSuccess){
        printf("KLF in %s\n", __FILE__);
        exit(-1);
      } 
#endif
    }
  }

#ifdef CUDAMSG
  // more timing stuff and conversion into reasonable units
  // for display
  cudaEventRecord(stop,  0);
  cudaEventSynchronize(stop);

  float elapsedTime;
  cudaEventElapsedTime(&elapsedTime, start, stop);

  cudaEventDestroy(start);
  cudaEventDestroy(stop);
 
  // double nnumb =  niter * (double) (nx * ny * nz); 
  // double nbyte =  nnumb * sizeof(double);
 
  // printf("XU Packed %g numbers in %g ms, BW = %g GB/s\n", 
  //     nnumb, elapsedTime, (nbyte/(elapsedTime/1e3))/1e9);

  printf("XU Packed numbers in %g ms\n", 
         elapsedTime);
#endif
} 

