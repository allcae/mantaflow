/******************************************************************************
 *
 * MantaFlow fluid solver framework
 * Copyright 2011 Tobias Pfaff, Nils Thuerey 
 *
 * This program is free software, distributed under the terms of the
 * GNU General Public License (GPL) 
 * http://www.gnu.org/licenses
 *
 * Vortex filament
 *
 ******************************************************************************/

#include "vortexfilament.h"
#include "integrator.h"
#include "interpol.h"
#include "mesh.h"
#include "quaternion.h"

using namespace std;
namespace Manta {

void VortexRing::renumber(int *_renumber) {
    for (size_t i=0; i<indices.size(); i++)
        indices[i] = _renumber[indices[i]];
}
    
class FilamentIntegrator : public PointSetIntegrator<vector<Vec3> > {
public:
    FilamentIntegrator(Real dt, Real regularization, Real cutoff, Real scale, const vector<VortexRing>& rings) :
        mDt(dt), mCutoff2(square(cutoff)), mA2(square(regularization), mRings(rings) {
            mStrength = 0.25 / M_PI * scale * dt;
        }
    
    void eval(const vector<Vec3>& x, const vector<Vec3>& y0, vector<Vec3>& u) const {
        filamentKernel(x, y0, u, *this);
    }
    
    Vec3 kernel(const vector<Vec3>& y, const Vec3& xi) const {
        Vec3 u(_0);
        for (size_t i=0; i<mRings.size(); i++) {
            const VortexRing& r = mRings[i];
            if (r.flag & ParticleBase::PDELETE) continue;
            
            const int N = r.size();
            const Real str = mStrength * r.circulation;
            for (size_t j=0; j<N; j++) {
                const Vec3 r0 = y[r.idx0(j)] - xi;
                const Vec3 r1 = y[r.idx1(j)] - xi;
                const Real r0_2 = normSquare(r0), r1_2 = normSquare(r1);
                if (r0_2 > mCutoff2 || r1_2 > mCutoff2 || r0_2 < 1e-6 || r1_2 < 1e-6)
                    continue;
                
                const Vec3 e = getNormalized(r1-r0);
                const Real r0n = 1.0f/sqrt(a2+r0_2);
                const Real r1n = 1.0f/sqrt(a2+r1_2);
                const Vec3 cp = cross(r0,e);
                const Real A = str * (dot(r1,e)*r1n - dot(r0,e)*r0n) / (a2 + normSquare(cp));
                u += A * cp;
            }
        }
        return u;
    }
    
    Real mDt, mCutoff2, mA2, mStrength;
    const vector<VortexRing>& mRings;
};

KERNEL (particle)
void filamentKernel(const vector<Vec3>& x, const vector<Vec3>& y0, vector<Vec3>& u, const FilamentIntegrator& fi) {
    u[i] = fi.kernel(y0,x[i]);
}

void VortexFilamentSystem::advectSelf(Real scale, Real regularization, int integrationMode) {
    const Real cutoff = 1e7;
    // backup
    vector<Vec3> pos(size());
    for (i=0; i<pos.size(); i++)
        pos[i] = mData[i].pos;
    
    FilamentIntegrator fi(getParent90->getDt(), regularization, cutoff, scale, mSegments);
    fi.integrate(pos, pos, integrationMode);
    
    for (int i=0; i<size(); i++)
        mData[i].pos = pos[i];
}

void VortexFilamentSystem::applyToMesh(Mesh& mesh, Real scale, Real regularization, int integrationMode) {
    // copy node array
    const int nodes = mesh.numNodes();
    vector<Vec3> nodes(nodes);
    for (int i=0; i<nodes; i++)
        nodes[i] = mesh.nodes(i).pos;
    
    FilamentIntegrator fi(getParent90->getDt(), regularization, cutoff, scale, mSegments);
    fi.integrate(pos, pos, integrationMode);
    
    // copy back
    for (int i=0; i<nodes; i++) {
        if (!mesh.isNodeFixed(i))
            mesh.nodes(i).pos = nodes[i];
    }    
}

void VortexFilamentSystem::remesh(Real maxLen) {
    const Real maxLen2 = maxLen*maxLen;
    
    for (int i=0; i < segSize(); i++) {
        for(;;) {
            map<int,int> insert;        
            VortexRing& r = mSegments[i];
            const int oldLen = r.size();
            int offset = 1;
            for (int j=0; j<oldLen; j++) {
                const Vec3 p0 = mData[r.idx0(j)].pos;
                const Vec3 p1 = mData[r.idx1(j)].pos;
                const Real l2 = normSquare(p1-p0);
                
                if (l2 > maxLen2) {
                    // insert midpoint
                    const Vec3 p_1 = mData[r.idx(j-1)].pos;
                    const Vec3 p2 = mData[r.idx(j+2)].pos;
                    const Vec3 mp = hermiteSpline(p0,p1,crTangent(p_1,p0,p1),crTangent(p0,p1,p2), 0.5);
                    insert.insert(pair<int,int>(j+offset, add(mp)));
                    offset++;
                }
            }
            if (insert.empty()) 
                break;
            
            // renumber indices
            const int newLen = oldLen + insert.size();
            int num=oldLen-1;
            r.indices.resize(newLen);
            for (int j=newLen-1; j>=0; j--) {
                map<int,int>::const_iterator f = insert.find(j);
                if (f==insert.end())
                    r.indices[j] = r.indices[num--];
                else
                    r.indices[j] = f->second;
            }
        }
    }
}

VortexFilamentSystem::VortexFilamentSystem(FluidSolver* parent) :
    ConnectedParticleSystem<BasicParticleData, VortexRing>(parent)
{     
}

ParticleBase* VortexFilamentSystem::clone() {
    VortexFilamentSystem* nm = new VortexFilamentSystem(getParent());
    compress();
    
    nm->mData = mData;
    nm->mSegments = mSegments;
    nm->setName(getName());
    return nm;
}

// ------------------------------------------------------------------------------
// Functions needed for doubly-discrete smoke flow using Darboux transforms
// see [Weissmann,Pinkall 2009]
// ------------------------------------------------------------------------------

Real evaluateRefU(int N, Real L, Real circ, Real reg) {
    // construct regular n-polygon
    const Real l = L/(Real)N;
    const Real r = 0.5*l/sin(M_PI/(Real)N);
    
    vector<Vec3> pos(N);
    for(int i=0; i<N; i++) {
        pos[i] = Vec3( r*cos(2.0*M_PI*(Real)i/N), r*sin(2.0*M_PI*(Real)i/N), 0);
    }
    
    // evaluate impact on pos[0]
    Vec3 sum(0.);
    for(int i=1; i<N-1; i++) {
        FilamentKernel kernel (pos[i], pos[i+1], circ, 1.0, 1e10, reg);
        sum += kernel.evaluate(pos[0]);
    }
    return norm(sum);
}

Vec3 darbouxStep(const Vec3& Si, const Vec3& lTi, Real r) {
    Quaternion rlTS (lTi - Si, -r);
    Quaternion lT (lTi, 0);
    Quaternion lTnext = rlTS * lT * rlTS.inverse();
    return lTnext.imag();
}

Vec3 monodromy(const vector<Vec3>& gamma, const Vec3& lT_1, Real r) {
    const int N = gamma.size();
    Vec3 lT (lT_1);
    
    for (int i=0; i<N; i++) {
        Vec3 Si = gamma[(i+1)%N]-gamma[i];
        lT = darbouxStep(Si, lT, r);
    }
    return lT;
}

bool powerMethod(const vector<Vec3>& gamma, Real l, Real r, Vec3& lT) {
    const int maxIter = 100;
    const Real epsilon = 1e-4;
    
    lT = Vec3(0,0,l);
    for (int i=0; i<maxIter; i++) {
        Vec3 lastLT (lT);
        lT = monodromy(gamma, lT, r);
        //if ((i%1) == 0) cout << "iteration " << i << " residual: " << norm(lT-lastLT) << endl;
        if (norm(lT-lastLT) < epsilon) 
            return true;
    }   
    return false;
}

bool darboux(const vector<Vec3>& from, vector<Vec3>& to, Real l, Real r) {
    const int N = from.size();
    Vec3 lT(0.);
    if (!powerMethod(from, l, r, lT))
        return false;
    
    for (int i=0; i<N; i++) {
        to[i] = from[i] + lT;
        Vec3 Si = from[(i+1)%N] - from[i];
        lT = darbouxStep(Si, lT, r);
    }
    return true;
}
        

void VortexFilamentSystem::doublyDiscreteUpdate(Real reg) {
    const Real dt = getParent()->getDt();
    
    for (int rc=0; rc<segSize(); rc++) {
        if (!isSegActive(rc)) continue;
        
         VortexRing& r = mSegments[rc];
         int N = r.size();
        
        // compute arc length
        Real L=0;
        for (int i=0; i<N; i++)
            L += norm(mData[r.idx0(i)].pos - mData[r.idx1(i)].pos);
        
        // build gamma
        vector<Vec3> gamma(N);
        for (int i=0; i<N; i++) gamma[i] = mData[r.indices[i]].pos;
        
        //N=1000; L=2.0*M_PI; reg=0.1; r.circulation=1;
        
        // compute reference parameters 
        const Real U = 0.5*r.circulation/L * (log(4.0*L/(M_PI*reg)) - 1.0);
        const Real Ur = evaluateRefU(N, L, r.circulation, reg);
        const Real d = 0.5*dt*(U-Ur);
        const Real l = sqrt( square(L/N) + square(d) );
        const Real ra = d*tan(M_PI * (0.5 - 1.0/N)); // d*cot(pi/n)
        
        // fwd darboux transform
        vector<Vec3> eta(N);
        if (!darboux(gamma, eta, l, ra)) {
            cout << "Fwd Darboux correction failed, skipped." << endl; 
            continue;
        }
        
        // bwd darboux transform
        if (!darboux(eta, gamma, l, -ra)) {
            cout << "Bwd Darboux correction failed, skipped." << endl; 
            continue;
        }
        
        // copy back
        for (int i=0; i<N; i++) {
            mData[r.indices[i]].pos = gamma[i];
        }
    }
}

void VortexFilamentSystem::addRing(const Vec3& position, Real circulation, Real radius, Vec3 normal, int number) {
    normalize(normal);
    Vec3 worldup (0,1,0);
    if (norm(normal - worldup) < 1e-5) worldup = Vec3(1,0,0);
    
    Vec3 u = cross(normal, worldup); normalize(u);
    Vec3 v = cross(normal, u); normalize(v);
    
    VortexRing ring(circulation);
    
    for (int i=0; i<number; i++) {
        Real phi = (Real)i/(Real)number * M_PI * 2.0;
        Vec3 p = position + radius * (u*cos(phi) + v*sin(phi));
        
        int num = add(BasicParticleData(p));
        ring.indices.push_back(num);
    }
    mSegments.push_back(ring);
}

    
} // namespace
