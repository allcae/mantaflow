/******************************************************************************
 *
 * MantaFlow fluid solver framework
 * Copyright 2011 Tobias Pfaff, Nils Thuerey 
 *
 * This program is free software, distributed under the terms of the
 * GNU General Public License (GPL) 
 * http://www.gnu.org/licenses
 *
 * Base class for particle systems
 *
 ******************************************************************************/

#ifndef _PARTICLE_H
#define _PARTICLE_H

#include <vector>
#include "grid.h"
#include "vectorbase.h"
#include "integrator.h"
namespace Manta {
// fwd decl
template<class T> class Grid;

//! Baseclass for particle systems. Does not implement any data
PYTHON class ParticleBase : public PbClass {
public:
    enum SystemType { BASE=0, PARTICLE, VELPART, VORTEX, FILAMENT, FLIP, TRACER };
    enum ParticleType {
        PNONE         = 0,
        PDELETE       = (1<<10), // mark as deleted, will be deleted in next compress() step
        PINVALID     = (1<<30), // unused
    };
    
    PYTHON ParticleBase(FluidSolver* parent) : PbClass(parent) {}

    virtual SystemType getType() const { return BASE; }
    virtual ParticleBase* clone() { return NULL; }
    virtual std::string infoString() { return "ParticleSystem " + mName + "<no info>"; };
};


//! Main class for particle systems
/*! Basetype S must at least contain flag, pos fields */
PYTHON template<class S> class ParticleSystem : public ParticleBase {
public:    
    PYTHON ParticleSystem(FluidSolver* parent) : ParticleBase(parent), mDeletes(0), mDeleteChunk(0) {}
    virtual ~ParticleSystem() {}
    
    virtual SystemType getType() const { return S::getType(); };
    
    // accessors
    inline S& operator[](int i) { return mData[i]; }
    inline const S& operator[](int i) const { return mData[i]; }
    inline int size() const { return mData.size(); }    
        
    // adding and deleting
    inline void kill(int i) { mData[i].flag |= PDELETE; if (++mDeletes > mDeleteChunk) compress(); }
    inline bool isActive(int i) { return (mData[i].flag & PDELETE) == 0; }    
    int add(const S& data);
    void clear();
    
    //! Advect particle in grid velocity field
    PYTHON void advectInGrid(FlagGrid& flaggrid, MACGrid& vel, int integrationMode);
    
    virtual ParticleBase* clone();
    virtual std::string infoString();
    
    protected:  
    virtual void compress();
    
    int mDeletes, mDeleteChunk;    
    std::vector<S> mData;    
};


//! Particle set with connectivity
PYTHON template<class DATA, class CON> 
class ConnectedParticleSystem : public ParticleSystem<DATA> {
public:
    PYTHON ConnectedParticleSystem(FluidSolver* parent) : ParticleSystem<DATA>(parent) {}
    
    // accessors
    inline bool isSegActive(int i) { return (mSegments[i].flag & ParticleBase::PDELETE) == 0; }    
    inline int segSize() const { return mSegments.size(); }    
    inline CON& seg(int i) { return mSegments[i]; }
    inline const CON& seg(int i) const { return mSegments[i]; }
        
    virtual ParticleBase* clone();
    
protected:
    std::vector<CON> mSegments;
    virtual void compress();    
};


//! Simplest data class for particle systems
struct BasicParticleData {
    BasicParticleData() : pos(0.), flag(0) {}
    BasicParticleData(const Vec3& p) : pos(p), flag(0) {}
    Vec3 pos;
    int flag;
    static ParticleBase::SystemType getType() { return ParticleBase::PARTICLE; }
};






//******************************************************************************
// Implementation
//******************************************************************************

const int DELETE_PART = 20; // chunk size for compression
   
template<class S>
void ParticleSystem<S>::clear() {
    mDeleteChunk = mDeletes = 0;
    mData.clear();
}

template<class S>
int ParticleSystem<S>::add(const S& data) {
    mData.push_back(data); 
    mDeleteChunk = mData.size() / DELETE_PART;
    return mData.size()-1;
}

KERNEL(pts) template<class S, IntegrationMode mode>
void KnAdvectInGrid(ParticleSystem<S>& p, MACGrid& vel, FlagGrid& flaggrid, Real dt) {
    if (!p.isActive(i)) return;
    
    // from integrator.h
    p[i].pos += integrateMeshMAC<mode>(p[i].pos, vel, dt);
    
    // TODO: else if(flaggrid.isObstacle(pos)) reproject
    if ((!flaggrid.isInBounds(p[i].pos,1) || flaggrid.isObstacle(p[i].pos)) && p[i].pos.x > 5)
        p[i].flag |= ParticleBase::PDELETE;
}

// advection plugin
template<class S>
void ParticleSystem<S>::advectInGrid(FlagGrid& flaggrid, MACGrid& vel, int integrationMode) {
    const Real dt = mParent->getDt();
    switch((IntegrationMode)integrationMode) {
        case EULER: KnAdvectInGrid<S, EULER>(*this, vel, flaggrid, dt); break;
        case RK2: KnAdvectInGrid<S, RK2>(*this, vel, flaggrid, dt); break;
        case RK4: KnAdvectInGrid<S, RK4>(*this, vel, flaggrid, dt); break;
        default: throw Error("invalid integration mode");
    }
}

template<class S>
void ParticleSystem<S>::compress() {
    int nextRead = mData.size();
    for (size_t i=0; i<mData.size(); i++) {
        while ((mData[i].flag & PDELETE) != 0) {
            nextRead--;
            mData[i] = mData[nextRead];
            mData[nextRead].flag = 0;           
        }
    }
    mData.resize(nextRead);
    mDeletes = 0;
    mDeleteChunk = mData.size() / DELETE_PART;
}

template<class DATA, class CON>
void ConnectedParticleSystem<DATA,CON>::compress() {
    const int sz = ParticleSystem<DATA>::size();
    int *renumber_back = new int[sz];
    int *renumber = new int[sz];
    for (int i=0; i<sz; i++)
        renumber[i] = renumber_back[i] = -1;
        
    // reorder elements
    std::vector<DATA>& data = ParticleSystem<DATA>::mData;
    int nextRead = sz;
    for (int i=0; i<nextRead; i++) {
        if ((data[i].flag & ParticleBase::PDELETE) != 0) {
            nextRead--;
            data[i] = data[nextRead];
            data[nextRead].flag = 0;           
            renumber_back[i] = nextRead;
        } else 
            renumber_back[i] = i;
    }
    
    // acceleration structure
    for (int i=0; i<nextRead; i++)
        renumber[renumber_back[i]] = i;
    
    // rename indices in filaments
    for (size_t i=0; i<mSegments.size(); i++)
        mSegments[i].renumber(renumber);
        
    ParticleSystem<DATA>::mData.resize(nextRead);
    ParticleSystem<DATA>::mDeletes = 0;
    ParticleSystem<DATA>::mDeleteChunk = ParticleSystem<DATA>::size() / DELETE_PART;
    
    delete[] renumber;
    delete[] renumber_back;
}

template<class S>
ParticleBase* ParticleSystem<S>::clone() {
    ParticleSystem<S>* nm = new ParticleSystem<S>(getParent());
    compress();
    
    nm->mData = mData;
    nm->setName(getName());
    return nm;
}

template<class DATA,class CON>
ParticleBase* ConnectedParticleSystem<DATA,CON>::clone() {
    ConnectedParticleSystem<DATA,CON>* nm = new ConnectedParticleSystem<DATA,CON>(this->getParent());
    compress();
    
    nm->mData = this->mData;
    nm->mSegments = mSegments;
    nm->setName(this->getName());
    return nm;
}

template<class S>
std::string ParticleSystem<S>::infoString() { 
    std::stringstream s;
    s << "ParticleSystem '" << getName() << "' [" << size() << " parts]";
    return s.str();
}
    
    
} // namespace





#endif