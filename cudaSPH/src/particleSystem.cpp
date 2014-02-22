/*
 * Copyright 1993-2013 NVIDIA Corporation.  All rights reserved.
 *
 * Please refer to the NVIDIA end user license agreement (EULA) associated
 * with this source code for terms and conditions that govern your use of
 * this software. Any use, reproduction, disclosure, or distribution of
 * this software and related documentation outside the terms of the EULA
 * is strictly prohibited.
 *
 */

#include "particleSystem.h"
#include "particleSystem.cuh"
#include "particles_kernel.cuh"

#include <cuda_runtime.h>

#include <helper_functions.h>
#include <helper_cuda.h>

#include <assert.h>
#include <math.h>
#include <memory.h>
#include <cstdio>
#include <cstdlib>
#include <algorithm>

#ifndef CUDART_PI_F
#define CUDART_PI_F         3.141592654
#endif

ParticleSystem::ParticleSystem(uint numParticles, uint3 gridSize) :
    m_bInitialized(false),
    m_numParticles(numParticles),
    m_hPos(0),
    m_hVel(0),
    m_dPos(0),
    m_dVel(0),
    m_gridSize(gridSize),
    m_timer(NULL),
    m_solverIterations(1)
{
    m_numGridCells = m_gridSize.x*m_gridSize.y*m_gridSize.z;
    double3 worldSize = make_double3(2.0, 2.0, 2.0);

    m_gridSortBits = 18;    // increase this for larger grids

    // set simulation parameters
    m_params.gridSize = m_gridSize;
    m_params.numCells = m_numGridCells;
    m_params.numBodies = m_numParticles;

    m_params.particleRadius = 1.0 / 64.0;
    m_params.colliderPos = make_double3(-1.2, -0.8, 0.8);
    m_params.colliderRadius = 0.2;

    m_params.worldOrigin = make_double3(-1.0, -1.0, -1.0);
    //    m_params.cellSize = make_double3(worldSize.x / m_gridSize.x, worldSize.y / m_gridSize.y, worldSize.z / m_gridSize.z);
    double cellSize = m_params.particleRadius * 2.0;  // cell size equal to particle diameter
    m_params.cellSize = make_double3(cellSize, cellSize, cellSize);

    m_params.spring = 0.5;
    m_params.damping = 0.02;
    m_params.shear = 0.1;
    m_params.attraction = 0.0;
    m_params.boundaryDamping = -0.5;

    m_params.gravity = make_double3(0.0, -0.0003, 0.0);
    m_params.globalDamping = 1.0;

    _initialize(numParticles);
}

ParticleSystem::~ParticleSystem()
{
    _finalize();
    m_numParticles = 0;
}

inline double lerp(double a, double b, double t)
{
    return a + t*(b-a);
}

void
ParticleSystem::_initialize(int numParticles)
{
    assert(!m_bInitialized);

    m_numParticles = numParticles;

    // allocate host storage
    m_hPos = new double[m_numParticles*4];
    m_hVel = new double[m_numParticles*4];
    memset(m_hPos, 0, m_numParticles*4*sizeof(double));
    memset(m_hVel, 0, m_numParticles*4*sizeof(double));

    m_hCellStart = new uint[m_numGridCells];
    memset(m_hCellStart, 0, m_numGridCells*sizeof(uint));

    m_hCellEnd = new uint[m_numGridCells];
    memset(m_hCellEnd, 0, m_numGridCells*sizeof(uint));

    // allocate GPU data
    unsigned int memSize = sizeof(double) * 4 * m_numParticles;

	allocateArray((void **)&m_dPos, memSize);
    allocateArray((void **)&m_dVel, memSize);

    allocateArray((void **)&m_dSortedPos, memSize);
    allocateArray((void **)&m_dSortedVel, memSize);

    allocateArray((void **)&m_dGridParticleHash, m_numParticles*sizeof(uint));
    allocateArray((void **)&m_dGridParticleIndex, m_numParticles*sizeof(uint));

    allocateArray((void **)&m_dCellStart, m_numGridCells*sizeof(uint));
    allocateArray((void **)&m_dCellEnd, m_numGridCells*sizeof(uint));

    sdkCreateTimer(&m_timer);

    setParameters(&m_params);

    m_bInitialized = true;
}

void
ParticleSystem::_finalize()
{
    assert(m_bInitialized);

    delete [] m_hPos;
    delete [] m_hVel;
    delete [] m_hCellStart;
    delete [] m_hCellEnd;

    freeArray(m_dVel);
    freeArray(m_dSortedPos);
    freeArray(m_dSortedVel);

    freeArray(m_dGridParticleHash);
    freeArray(m_dGridParticleIndex);
    freeArray(m_dCellStart);
    freeArray(m_dCellEnd);
}

// step the simulation
void
ParticleSystem::update(double deltaTime)
{
    assert(m_bInitialized);

    // update constants
    setParameters(&m_params);

    // integrate
    integrateSystem(
        m_dPos,
        m_dVel,
        deltaTime,
        m_numParticles);

    // calculate grid hash
    calcHash(
        m_dGridParticleHash,
        m_dGridParticleIndex,
        m_dPos,
        m_numParticles);

    // sort particles based on hash
    sortParticles(m_dGridParticleHash, m_dGridParticleIndex, m_numParticles);

    // reorder particle arrays into sorted order and
    // find start and end of each cell
    reorderDataAndFindCellStart(
        m_dCellStart,
        m_dCellEnd,
        m_dSortedPos,
        m_dSortedVel,
        m_dGridParticleHash,
        m_dGridParticleIndex,
        m_dPos,
        m_dVel,
        m_numParticles,
        m_numGridCells);

    // process collisions
    collide(
        m_dVel,
        m_dSortedPos,
        m_dSortedVel,
        m_dGridParticleIndex,
        m_dCellStart,
        m_dCellEnd,
        m_numParticles,
        m_numGridCells);

}

void
ParticleSystem::dumpGrid()
{
    // dump grid information
    copyArrayFromDevice(m_hCellStart, m_dCellStart, 0, sizeof(uint)*m_numGridCells);
    copyArrayFromDevice(m_hCellEnd, m_dCellEnd, 0, sizeof(uint)*m_numGridCells);
    uint maxCellSize = 0;

    for (uint i=0; i<m_numGridCells; i++)
    {
        if (m_hCellStart[i] != 0xffffffff)
        {
            uint cellSize = m_hCellEnd[i] - m_hCellStart[i];

            //            printf("cell: %d, %d particles\n", i, cellSize);
            if (cellSize > maxCellSize)
            {
                maxCellSize = cellSize;
            }
        }
    }

    printf("maximum particles per cell = %d\n", maxCellSize);
}

void
ParticleSystem::dumpParticles(uint start, uint count)
{
    // debug
    copyArrayFromDevice(m_hPos, m_dPos, 0, sizeof(double)*4*count);
    copyArrayFromDevice(m_hVel, m_dVel, 0, sizeof(double)*4*count);

    for (uint i=start; i<start+count; i++)
    {
        //        printf("%d: ", i);
        printf("pos: (%.4f, %.4f, %.4f, %.4f)\n", m_hPos[i*4+0], m_hPos[i*4+1], m_hPos[i*4+2], m_hPos[i*4+3]);
        printf("vel: (%.4f, %.4f, %.4f, %.4f)\n", m_hVel[i*4+0], m_hVel[i*4+1], m_hVel[i*4+2], m_hVel[i*4+3]);
    }
}

double *
ParticleSystem::getArray(ParticleArray array)
{
    assert(m_bInitialized);

    double *hdata = 0;
    double *ddata = 0;

    switch (array)
    {
        default:
        case POSITION:
            hdata = m_hPos;
            ddata = m_dPos;
            break;

        case VELOCITY:
            hdata = m_hVel;
            ddata = m_dVel;
            break;
    }

    copyArrayFromDevice(hdata, ddata, m_numParticles*4*sizeof(double));
    return hdata;
}

void
ParticleSystem::setArray(ParticleArray array, const double *data, int start, int count)
{
    assert(m_bInitialized);

    switch (array)
    {
        default:
        case POSITION:
			copyArrayToDevice(m_dPos, data, start*4*sizeof(double), count*4*sizeof(double));
			break;



        case VELOCITY:
            copyArrayToDevice(m_dVel, data, start*4*sizeof(double), count*4*sizeof(double));
            break;
    }
}

inline double frand()
{
    return rand() / (double) RAND_MAX;
}

void
ParticleSystem::initGrid(uint *size, double spacing, double jitter, uint numParticles)
{
    srand(1973);

    for (uint z=0; z<size[2]; z++)
    {
        for (uint y=0; y<size[1]; y++)
        {
            for (uint x=0; x<size[0]; x++)
            {
                uint i = (z*size[1]*size[0]) + (y*size[0]) + x;

                if (i < numParticles)
                {
                    m_hPos[i*4] = (spacing * x) + m_params.particleRadius - 1.0 + (frand()*2.0-1.0)*jitter;
                    m_hPos[i*4+1] = (spacing * y) + m_params.particleRadius - 1.0 + (frand()*2.0-1.0)*jitter;
                    m_hPos[i*4+2] = (spacing * z) + m_params.particleRadius - 1.0 + (frand()*2.0-1.0)*jitter;
                    m_hPos[i*4+3] = 1.0;

                    m_hVel[i*4] = 0.0;
                    m_hVel[i*4+1] = 0.0;
                    m_hVel[i*4+2] = 0.0;
                    m_hVel[i*4+3] = 0.0;
                }
            }
        }
    }
}

void
ParticleSystem::reset(ParticleConfig config)
{
    switch (config)
    {
        default:
        case CONFIG_RANDOM:
            {
                int p = 0, v = 0;

                for (uint i=0; i < m_numParticles; i++)
                {
                    double point[3];
                    point[0] = frand();
                    point[1] = frand();
                    point[2] = frand();
                    m_hPos[p++] = 2 * (point[0] - 0.5);
                    m_hPos[p++] = 2 * (point[1] - 0.5);
                    m_hPos[p++] = 2 * (point[2] - 0.5);
                    m_hPos[p++] = 1.0; // radius
                    m_hVel[v++] = 0.0;
                    m_hVel[v++] = 0.0;
                    m_hVel[v++] = 0.0;
                    m_hVel[v++] = 0.0;
                }
            }
            break;

        case CONFIG_GRID:
            {
                double jitter = m_params.particleRadius*0.01;
                uint s = (int) ceil(pow((double) m_numParticles, 1.0 / 3.0));
                uint gridSize[3];
                gridSize[0] = gridSize[1] = gridSize[2] = s;
                initGrid(gridSize, m_params.particleRadius*2.0, jitter, m_numParticles);
            }
            break;
    }

    setArray(POSITION, m_hPos, 0, m_numParticles);
    setArray(VELOCITY, m_hVel, 0, m_numParticles);
}
