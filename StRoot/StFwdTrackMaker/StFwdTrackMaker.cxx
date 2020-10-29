#include "StFwdTrackMaker/StFwdTrackMaker.h"
#include "StFwdTrackMaker/include/Tracker/FwdHit.h"
#include "StFwdTrackMaker/include/Tracker/FwdTracker.h"
#include "StFwdTrackMaker/include/Tracker/TrackFitter.h"
#include "StFwdTrackMaker/include/Tracker/FwdGeomUtils.h"

#include "KiTrack/IHit.h"
#include "GenFit/Track.h"

#include "TMath.h"

#include <limits>
#include <map>
#include <string>
#include <string>
#include <vector>

#include "StEvent/StEvent.h"
#include "StEvent/StGlobalTrack.h"
#include "StEvent/StHelixModel.h"
#include "StEvent/StPrimaryTrack.h"
#include "StEvent/StRnDHit.h"
#include "StEvent/StRnDHitCollection.h"
#include "StEvent/StTrack.h"
#include "StEvent/StTrackGeometry.h"
#include "StEvent/StTrackNode.h"
#include "StEvent/StPrimaryVertex.h"
#include "StEvent/StEnumerations.h"
#include "StEvent/StTrackDetectorInfo.h"

#include "StEventUtilities/StEventHelper.h"

#include "tables/St_g2t_fts_hit_Table.h"
#include "tables/St_g2t_track_Table.h"

#include "StarMagField/StarMagField.h"

#include "St_base/StMessMgr.h"
#include "StarClassLibrary/StPhysicalHelix.hh"
#include "StarClassLibrary/SystemOfUnits.h"

#include <SystemOfUnits.h>
#include <exception>

#include "TROOT.h"
#include "TLorentzVector.h"

FwdSystem* FwdSystem::sInstance = nullptr;

//_______________________________________________________________________________________
// For now, accept anything we are passed, no matter what it is or how bad it is
template<typename T> bool accept( T ) { return true; }

// Basic sanity cuts on genfit tracks
template<> bool accept( genfit::Track *track )
{
    // This also gets rid of failed fits (but may need to explicitly
    // for fit failure...)
    if (track->getNumPoints() <= 0 ) return false; // fit may have failed

    auto cardinal = track->getCardinalRep();

    // Check that the track fit converged
    auto status = track->getFitStatus( cardinal );
    if ( 0 == status->isFitConverged() ) {
      return false;
    }


    // Next, check that all points on the track have fitter info
    // (may be another indication of a failed fit?)
    for ( auto point : track->getPoints() ) {
      if ( !point->hasFitterInfo(cardinal) ) {
	return false;
      }
    }

    // Following line fails with an exception, because some tracks lack 
    //   forward update, or prediction in fitter info at the first point
    //
    // genfit::KalmanFitterInfo::getFittedState(bool) const of 
    //                         GenFit/fitters/src/KalmanFitterInfo.cc:250
  
    // Fitted state at the first point
    // const auto &atFirstPoint = track->getFittedState();

    // Getting the fitted state from a track occasionally fails, because
    // the first point on the fit doesn't have forward/backward fit
    // information.  So we want the first point with fit info...
 
    genfit::TrackPoint* first = 0;
    unsigned int ipoint = 0;
    for ( ipoint = 0; ipoint < track->getNumPoints(); ipoint++ ) {
      first = track->getPointWithFitterInfo( ipoint );
      if ( first ) break;
    } 
  
    // No points on the track have fit information
    if ( 0 == first ) {
      LOG_INFO << "No fit information on track" << endm;
      return false;
    }

    auto& fittedState= track->getFittedState(ipoint);

    TVector3 momentum = fittedState.getMom();
    double   pt       = momentum.Perp();

    if (pt < 0.10 ) return false; // below this

    return true;
 
};

//_______________________________________________________________________________________
// // Truth handlers
int TheTruth ( const Seed_t& seed, int &qa ) {

  int count = 0;
  std::map<int,int> truth;
  for ( auto hit : seed ) {
    count++; // add another hit
    FwdHit* fhit = dynamic_cast<FwdHit*>(hit);
    if ( 0 == fhit ) continue;
    truth[ fhit->_tid ]++;
  }

  int id = -1;
  int nmax = -1;
  for (auto const& it : truth ) {
    if ( it.second > nmax ) {
      nmax = it.second;
      id   = it.first;
    }
  }
  // QA is stored as an integer representing the percentage of hits which
  // vote the same way on the track
  qa = int( 100.0 * double(nmax) / double(count) );
  return id;

};

class SiRasterizer {
  public:
    SiRasterizer() {}
    SiRasterizer(FwdTrackerConfig &_cfg) { setup(_cfg); }
    ~SiRasterizer() {}
    void setup(FwdTrackerConfig &_cfg) {
        cfg = _cfg;
        mRasterR = cfg.get<double>("SiRasterizer:r", 3.0);
        mRasterPhi = cfg.get<double>("SiRasterizer:phi", 0.1);
    }

    bool active() {
        return cfg.get<bool>("SiRasterizer:active", false);
    }

    TVector3 raster(TVector3 p0) {
        TVector3 p = p0;
        double r = p.Perp();
        double phi = p.Phi();
        const double minR = 5.0;
        // 5.0 is the r minimum of the Si
        p.SetPerp(minR + (floor((r - minR) / mRasterR) * mRasterR + mRasterR / 2.0));
        p.SetPhi(-TMath::Pi() + (floor((phi + TMath::Pi()) / mRasterPhi) * mRasterPhi + mRasterPhi / 2.0));
        return p;
    }

    FwdTrackerConfig cfg;
    double mRasterR, mRasterPhi;
};

//  Wrapper class around the forward tracker
class ForwardTracker : public ForwardTrackMaker {
  public:
    // Replaces original initialization.  Config file and hitloader
    // will be provided by the maker.
    void initialize() {
        LOG_INFO << "ForwardTracker::initialize()" << endm;
        nEvents = 1; // only process single event

        // Create the forward system...
        FwdSystem::sInstance = new FwdSystem();

        // make our quality plotter
        mQualityPlotter = new QualityPlotter(mConfig);
        mQualityPlotter->makeHistograms(mConfig.get<size_t>("TrackFinder:nIterations", 1));

        // initialize the track fitter
        mTrackFitter = new TrackFitter(mConfig);
        mTrackFitter->setup();

        ForwardTrackMaker::initialize();
    }

    void finish() {

        if ( mGenHistograms ){
            mQualityPlotter->finish();
            writeEventHistograms();
        }

        if (FwdSystem::sInstance){
            delete FwdSystem::sInstance;
            FwdSystem::sInstance = 0;
        }
        if (mQualityPlotter){
            delete mQualityPlotter;
            mQualityPlotter = 0;
        }
        if (mTrackFitter){
            delete mTrackFitter;
            mTrackFitter= 0;
        }
    }
};

// Wrapper around the hit load.
class ForwardHitLoader : public IHitLoader {
  public:
    unsigned long long nEvents() { return 1; }
    std::map<int, std::vector<KiTrack::IHit *>> &load(unsigned long long) {
        return _hits;
    };
    std::map<int, std::vector<KiTrack::IHit *>> &loadSi(unsigned long long) {
        return _fsi_hits;
    };
    std::map<int, shared_ptr<McTrack>> &getMcTrackMap() {
        return _mctracks;
    };

    // Cleanup
    void clear() {
        _hits.clear();
        _fsi_hits.clear();
        _mctracks.clear();
    }

    // TODO, protect and add interaface for pushing hits / tracks
    std::map<int, std::vector<KiTrack::IHit *>> _hits;
    std::map<int, std::vector<KiTrack::IHit *>> _fsi_hits;
    std::map<int, shared_ptr<McTrack>> _mctracks;
};

//________________________________________________________________________
StFwdTrackMaker::StFwdTrackMaker() : StMaker("fwdTrack"), mForwardTracker(nullptr), mForwardHitLoader(nullptr), mGenHistograms(false), mGenTree(false){
    SetAttr("useFtt",1);                 // Default Ftt on 
    SetAttr("useFst",1);                 // Default Fst on
    SetAttr("config", "config.xml");     // Default configuration file (user may override before Init())
    SetAttr("fillEvent",1); // fill StEvent
};

int StFwdTrackMaker::Finish() {
    mForwardTracker->finish();
    
    if ( mGenHistograms ) {
        gDirectory->mkdir("StFwdTrackMaker");
        gDirectory->cd("StFwdTrackMaker");
        for (auto nh : mHistograms) {
            nh.second->SetDirectory(gDirectory);
            nh.second->Write();
        }
    }

    if (mGenTree) {
        mTree->Print();
        mTreeFile->cd();
        mTree->Write();
        mTreeFile->Write();
    }
    return kStOk;
}

//________________________________________________________________________
int StFwdTrackMaker::Init() {
    // Initialize configuration file
    std::string configFile = SAttr("config");
    if (mConfigFile.length() > 4) {
        configFile = mConfigFile;
        LOG_INFO << "Forward Tracker is using config file : " <<  mConfigFile << endm;
    }

    mFwdConfig.load( configFile );

    if (mGenTree) {
        mTreeFile = new TFile("mltree.root", "RECREATE");
        mTree = new TTree("Stg", "stg hits");
        mTree->Branch("n", &mTreeN, "n/I");
        mTree->Branch("x", mTreeX, "x[n]/F");
        mTree->Branch("y", mTreeY, "y[n]/F");
        mTree->Branch("z", mTreeZ, "z[n]/F");
        mTree->Branch("tid", &mTreeTID, "tid[n]/I");
        mTree->Branch("vid", &mTreeVID, "vid[n]/I");
        mTree->Branch("hpt", &mTreeHPt, "hpt[n]/F");
        mTree->Branch("hsv", &mTreeHSV, "hsv[n]/I");

        // mc tracks
        mTree->Branch("nt", &mTreeNTracks, "nt/I");
        mTree->Branch("pt", &mTreePt, "pt[nt]/F");
        mTree->Branch("eta", &mTreeEta, "eta[nt]/F");
        mTree->Branch("phi", &mTreePhi, "phi[nt]/F");
        mTree->Branch("tid", &mTreeTID, "tid/I");

        std::string path = "TrackFinder.Iteration[0].SegmentBuilder";
        std::vector<string> paths = mFwdConfig.childrenOf(path);

        for (string p : paths) {
            string name = mFwdConfig.get<string>(p + ":name", "");
            mTreeCrits[name]; // create the entry
            mTree->Branch(name.c_str(), &mTreeCrits[name]);
            mTree->Branch((name + "_trackIds").c_str(), &mTreeCritTrackIds[name]);
        }

        // Three hit criteria
        path = "TrackFinder.Iteration[0].ThreeHitSegments";
        paths = mFwdConfig.childrenOf(path);

        for (string p : paths) {
            string name = mFwdConfig.get<string>(p + ":name", "");
            mTreeCrits[name]; // create the entry
            mTree->Branch(name.c_str(), &mTreeCrits[name]);
            mTree->Branch((name + "_trackIds").c_str(), &mTreeCritTrackIds[name]);
        }

        mTree->SetAutoFlush(0);
    } // gen tree


    // create an SiRasterizer in case we need it 
    mSiRasterizer = std::shared_ptr<SiRasterizer>( new SiRasterizer(mFwdConfig));
    mForwardTracker = std::shared_ptr<ForwardTracker>(new ForwardTracker());
    mForwardTracker->setConfig(mFwdConfig);

    // only save criteria values if we are generating a tree.
    mForwardTracker->setSaveCriteriaValues(mGenTree);

    mForwardHitLoader = std::shared_ptr<ForwardHitLoader>(new ForwardHitLoader());
    mForwardTracker->setLoader(mForwardHitLoader);
    mForwardTracker->initialize();

    if ( mGenHistograms ){
        mHistograms["McEventEta"] = new TH1D("McEventEta", ";MC Track Eta", 1000, -5, 5);
        mHistograms["McEventPt"] = new TH1D("McEventPt", ";MC Track Pt (GeV/c)", 1000, 0, 10);
        mHistograms["McEventPhi"] = new TH1D("McEventPhi", ";MC Track Phi", 1000, 0, 6.2831852);

        // these are tracks within 2.5 < eta < 4.0
        mHistograms["McEventFwdEta"] = new TH1D("McEventFwdEta", ";MC Track Eta", 1000, -5, 5);
        mHistograms["McEventFwdPt"] = new TH1D("McEventFwdPt", ";MC Track Pt (GeV/c)", 1000, 0, 10);
        mHistograms["McEventFwdPhi"] = new TH1D("McEventFwdPhi", ";MC Track Phi", 1000, 0, 6.2831852);

        // create mHistograms
        mHistograms["nMcTracks"] = new TH1I("nMcTracks", ";# MC Tracks/Event", 1000, 0, 1000);
        mHistograms["nMcTracksFwd"] = new TH1I("nMcTracksFwd", ";# MC Tracks/Event", 1000, 0, 1000);
        mHistograms["nMcTracksFwdNoThreshold"] = new TH1I("nMcTracksFwdNoThreshold", ";# MC Tracks/Event", 1000, 0, 1000);

        mHistograms["nHitsSTGC"] = new TH1I("nHitsSTGC", ";# STGC Hits/Event", 1000, 0, 1000);
        mHistograms["nHitsFSI"] = new TH1I("nHitsFSI", ";# FSIT Hits/Event", 1000, 0, 1000);

        mHistograms["stgc_volume_id"] = new TH1I("stgc_volume_id", ";stgc_volume_id", 50, 0, 50);
        mHistograms["fsi_volume_id"] = new TH1I("fsi_volume_id", ";fsi_volume_id", 50, 0, 50);

        mHistograms["fsiHitDeltaR"] = new TH1F("fsiHitDeltaR", "FSI; delta r (cm); ", 500, -5, 5);
        mHistograms["fsiHitDeltaPhi"] = new TH1F("fsiHitDeltaPhi", "FSI; delta phi; ", 500, -5, 5);

        // there are 4 stgc stations
        for (int i = 0; i < 4; i++) {
            mHistograms[TString::Format("stgc%dHitMap", i).Data()] = new TH2F(TString::Format("stgc%dHitMap", i), TString::Format("STGC Layer %d; x (cm); y(cm)", i), 200, -100, 100, 200, -100, 100);

            mHistograms[TString::Format("stgc%dHitMapPrim", i).Data()] = new TH2F(TString::Format("stgc%dHitMapPrim", i), TString::Format("STGC Layer %d; x (cm); y(cm)", i), 200, -100, 100, 200, -100, 100);
            mHistograms[TString::Format("stgc%dHitMapSec", i).Data()] = new TH2F(TString::Format("stgc%dHitMapSec", i), TString::Format("STGC Layer %d; x (cm); y(cm)", i), 200, -100, 100, 200, -100, 100);
        }

        // There are 3 silicon stations
        for (int i = 0; i < 3; i++) {
            mHistograms[TString::Format("fsi%dHitMap", i).Data()] = new TH2F(TString::Format("fsi%dHitMap", i), TString::Format("FSI Layer %d; x (cm); y(cm)", i), 200, -100, 100, 200, -100, 100);
            mHistograms[TString::Format("fsi%dHitMapR", i).Data()] = new TH1F(TString::Format("fsi%dHitMapR", i), TString::Format("FSI Layer %d; r (cm); ", i), 500, 0, 50);
            mHistograms[TString::Format("fsi%dHitMapPhi", i).Data()] = new TH1F(TString::Format("fsi%dHitMapPhi", i), TString::Format("FSI Layer %d; phi; ", i), 320, 0, TMath::Pi() * 2 + 0.1);
        }

    } // mGenHistograms

    return kStOK;
};

TMatrixDSym makeSiCovMat(TVector3 hit, FwdTrackerConfig &xfg) {
    // we can calculate the CovMat since we know the det info, but in future we should probably keep this info in the hit itself

    float rSize = xfg.get<float>("SiRasterizer:r", 3.0);
    float phiSize = xfg.get<float>("SiRasterizer:phi", 0.004);

    // measurements on a plane only need 2x2
    // for Si geom we need to convert from cylindrical to cartesian coords
    TMatrixDSym cm(2);
    TMatrixD T(2, 2);
    TMatrixD J(2, 2);
    const float x = hit.X();
    const float y = hit.Y();
    const float R = sqrt(x * x + y * y);
    const float cosphi = x / R;
    const float sinphi = y / R;
    const float sqrt12 = sqrt(12.);

    const float dr = rSize / sqrt12;
    const float dphi = (phiSize) / sqrt12;

    // Setup the Transposed and normal Jacobian transform matrix;
    // note, the si fast sim did this wrong
    // row col
    T(0, 0) = cosphi;
    T(0, 1) = -R * sinphi;
    T(1, 0) = sinphi;
    T(1, 1) = R * cosphi;

    J(0, 0) = cosphi;
    J(0, 1) = sinphi;
    J(1, 0) = -R * sinphi;
    J(1, 1) = R * cosphi;

    TMatrixD cmcyl(2, 2);
    cmcyl(0, 0) = dr * dr;
    cmcyl(1, 1) = dphi * dphi;

    TMatrixD r = T * cmcyl * J;

    // note: float sigmaX = sqrt(r(0, 0));
    // note: float sigmaY = sqrt(r(1, 1));

    cm(0, 0) = r(0, 0);
    cm(1, 1) = r(1, 1);
    cm(0, 1) = r(0, 1);
    cm(1, 0) = r(1, 0);

    TMatrixDSym tamvoc(3);
    tamvoc( 0, 0 ) = cm(0, 0); tamvoc( 0, 1 ) = cm(0, 1); tamvoc( 0, 2 ) = 0.0;
    tamvoc( 1, 0 ) = cm(1, 0); tamvoc( 1, 1 ) = cm(1, 1); tamvoc( 1, 2 ) = 0.0;
    tamvoc( 2, 0 ) = 0.0;      tamvoc( 2, 1 ) = 0.0; tamvoc( 2, 2 )      = 0.01*0.01;


    return tamvoc;
}

void StFwdTrackMaker::loadStgcHits( std::map<int, shared_ptr<McTrack>> &mcTrackMap, std::map<int, std::vector<KiTrack::IHit *>> &hitMap, int count ){

    // Get the StEvent handle to see if the rndCollection is available
    StEvent *event = (StEvent *)this->GetDataSet("StEvent");
    StRnDHitCollection *rndCollection = nullptr;
    if (nullptr != event) {
        rndCollection = event->rndHitCollection();
    }

    string fttFromGEANT = mFwdConfig.get<string>( "Source:ftt", "" );

    if ( rndCollection == nullptr || "GEANT" == fttFromGEANT ){
        LOG_INFO << "Loading sTGC hits directly from GEANT hits" << endm;
        loadStgcHitsFromGEANT( mcTrackMap, hitMap, count );
    } else {
        LOG_INFO << "loading sTGC from StEvent" << endm;
        loadStgcHitsFromStEvent( mcTrackMap, hitMap, count );
    }
} // loadStgcHits

void StFwdTrackMaker::loadStgcHitsFromGEANT( std::map<int, shared_ptr<McTrack>> &mcTrackMap, std::map<int, std::vector<KiTrack::IHit *>> &hitMap, int count ){
    /************************************************************/
    // STGC Hits
    St_g2t_fts_hit *g2t_stg_hits = (St_g2t_fts_hit *)GetDataSet("geant/g2t_stg_hit");

    // make the Covariance Matrix once and then reuse
    TMatrixDSym hitCov3(3);
    double sigXY = 0.01;
    hitCov3(0, 0) = sigXY * sigXY;
    hitCov3(1, 1) = sigXY * sigXY;
    hitCov3(2, 2) = 0.0; // unused since they are loaded as points on plane

    int nstg = 0;
    if (g2t_stg_hits == nullptr) {
    } else {
        nstg = g2t_stg_hits->GetNRows();
    }

    LOG_INFO << "This event has " << nstg << " stg hits in geant/g2t_stg_hit " << endm;
    if ( mGenHistograms ) {
        this->mHistograms["nHitsSTGC"]->Fill(nstg);
    }
    this->mTreeN = 0;

    bool filterGEANT = mFwdConfig.get<bool>( "Source:fttFilter", false );
    for (int i = 0; i < nstg; i++) {

        g2t_fts_hit_st *git = (g2t_fts_hit_st *)g2t_stg_hits->At(i);
        if (0 == git)
            continue; // geant hit
        int track_id = git->track_p;
        int volume_id = git->volume_id;
        int plane_id = (volume_id - 1) / 4;           // from 1 - 16. four chambers per station
        float x = git->x[0] + gRandom->Gaus(0, 0.01); // 100 micron blur according to approx sTGC reso
        float y = git->x[1] + gRandom->Gaus(0, 0.01); // 100 micron blur according to approx sTGC reso
        float z = git->x[2];

        if (mGenTree) {
            mTreeX[mTreeN] = x;
            mTreeY[mTreeN] = y;
            mTreeZ[mTreeN] = z;
            mTreeTID[mTreeN] = track_id;
            mTreeVID[mTreeN] = plane_id;
            mTreeHPt[mTreeN] = mcTrackMap[track_id]->mPt;
            mTreeHSV[mTreeN] = mcTrackMap[track_id]->mStartVertex;
            mTreeN++;
        }

        if ( mGenHistograms ){
            this->mHistograms["stgc_volume_id"]->Fill(volume_id);
        }

        if (plane_id < 4 && plane_id >= 0) {
            if ( mGenHistograms ){
                this->mHistograms[TString::Format("stgc%dHitMap", plane_id).Data()]->Fill(x, y);
            }
        } else {
            continue;
        }

        // this rejects GEANT hits with eta -999 - do we understand this effect?
        if ( filterGEANT ) {
            if ( mcTrackMap[track_id] && fabs(mcTrackMap[track_id]->mEta) > 5.0 ){
                
                if ( mGenHistograms ) this->mHistograms[TString::Format("stgc%dHitMapSec", plane_id).Data()]->Fill(x, y);
                continue;
            } else if ( mcTrackMap[track_id] && fabs(mcTrackMap[track_id]->mEta) < 5.0 ){
                if ( mGenHistograms ) this->mHistograms[TString::Format("stgc%dHitMapPrim", plane_id).Data()]->Fill(x, y);
            }
        }

        FwdHit *hit = new FwdHit(count++, x, y, z, -plane_id, track_id, hitCov3, mcTrackMap[track_id]);

        // Add the hit to the hit map
        hitMap[hit->getSector()].push_back(hit);

        // Add hit pointer to the track
        if (mcTrackMap[track_id])
            mcTrackMap[track_id]->addHit(hit);
    }
} // loadStgcHits

void StFwdTrackMaker::loadStgcHitsFromStEvent( std::map<int, std::shared_ptr<McTrack>> &mcTrackMap, std::map<int, std::vector<KiTrack::IHit *>> &hitMap, int count ){

    // Get the StEvent handle
    StEvent *event = (StEvent *)this->GetDataSet("StEvent");
    if (0 == event) {
        return;
    }

    StRnDHitCollection *rndCollection = event->rndHitCollection();
    if (0 == rndCollection) {
        LOG_INFO << "No StRnDHitCollection found" << endm;
    }

    const StSPtrVecRnDHit &hits = rndCollection->hits();

    // we will reuse this to hold the cov mat
    TMatrixDSym hitCov3(3);
    
    for (unsigned int stgchit_index = 0; stgchit_index < hits.size(); stgchit_index++) {
        StRnDHit *hit = hits[stgchit_index];

        if ( hit->layer() <= 6 ){
            // skip FST hits here
            continue;
        }

        int layer = hit->layer() - 9;

        const StThreeVectorF pos = hit->position();

        StMatrixF covmat = hit->covariantMatrix();

        // copy covariance matrix element by element from StMatrixF
        hitCov3(0,0) = covmat[0][0]; hitCov3(0,1) = covmat[0][1]; hitCov3(0,2) = covmat[0][2];
        hitCov3(1,0) = covmat[1][0]; hitCov3(1,1) = covmat[1][1]; hitCov3(1,2) = covmat[1][2];
        hitCov3(2,0) = covmat[2][0]; hitCov3(2,1) = covmat[2][1]; hitCov3(2,2) = covmat[2][2];

        shared_ptr<McTrack> mct = nullptr;
        if ( hit->idTruth() > 0 && mcTrackMap.count( hit->idTruth() ) ){
            mct = mcTrackMap[hit->idTruth()];
        }
        FwdHit *fhit = new FwdHit(count++, hit->position().x(), hit->position().y(), hit->position().z(), -layer, hit->idTruth(), hitCov3, mct);

        // Add the hit to the hit map
        hitMap[fhit->getSector()].push_back(fhit);

        // Add hit pointer to the track
        if ( hit->idTruth() > 0 && mcTrackMap[hit->idTruth()])
            mcTrackMap[hit->idTruth()]->addHit(fhit);
    }
} //loadStgcHitsFromStEvent

void StFwdTrackMaker::loadFstHits( std::map<int, shared_ptr<McTrack>> &mcTrackMap, std::map<int, std::vector<KiTrack::IHit *>> &hitMap, int count ){

    // Get the StEvent handle to see if the rndCollection is available
    StEvent *event = (StEvent *)this->GetDataSet("StEvent");
    StRnDHitCollection *rndCollection = nullptr;
    if (nullptr != event) {
        rndCollection = event->rndHitCollection();
    }
    bool siRasterizer = mFwdConfig.get<bool>( "SiRasterizer:active", false );
    if ( siRasterizer || rndCollection == nullptr ){
        LOG_INFO << "Loading Fst hits from GEANT with SiRasterizer" << endm;
        loadFstHitsFromGEANT( mcTrackMap, hitMap, count );
    } else {
        LOG_INFO << "Loading Fst hits from StEvent" << endm;
        loadFstHitsFromStEvent( mcTrackMap, hitMap, count );
    }
} // loadFstHits

void StFwdTrackMaker::loadFstHitsFromStEvent( std::map<int, shared_ptr<McTrack>> &mcTrackMap, std::map<int, std::vector<KiTrack::IHit *>> &hitMap, int count ){

    // Get the StEvent handle
    StEvent *event = (StEvent *)this->GetDataSet("StEvent");
    if (0 == event) {
        return;
    }

    StRnDHitCollection *rndCollection = event->rndHitCollection();

    const StSPtrVecRnDHit &hits = rndCollection->hits();

    // we will reuse this to hold the cov mat
    TMatrixDSym hitCov3(3);
    
    for (unsigned int fsthit_index = 0; fsthit_index < hits.size(); fsthit_index++) {
        StRnDHit *hit = hits[fsthit_index];
    
        if ( hit->layer() > 6 ){
            // skip sTGC hits here
            continue;
        }

        const StThreeVectorF pos = hit->position();

        StMatrixF covmat = hit->covariantMatrix();

        // copy covariance matrix element by element from StMatrixF
        hitCov3(0,0) = covmat[0][0]; hitCov3(0,1) = covmat[0][1]; hitCov3(0,2) = covmat[0][2];
        hitCov3(1,0) = covmat[1][0]; hitCov3(1,1) = covmat[1][1]; hitCov3(1,2) = covmat[1][2];
        hitCov3(2,0) = covmat[2][0]; hitCov3(2,1) = covmat[2][1]; hitCov3(2,2) = covmat[2][2];

        FwdHit *fhit = new FwdHit(count++, hit->position().x(), hit->position().y(), hit->position().z(), hit->layer(), hit->idTruth(), hitCov3, mcTrackMap[hit->idTruth()]);

        // Add the hit to the hit map
        hitMap[fhit->getSector()].push_back(fhit);

    }
} //loadFstHitsFromStEvent

void StFwdTrackMaker::loadFstHitsFromGEANT( std::map<int, shared_ptr<McTrack>> &mcTrackMap, std::map<int, std::vector<KiTrack::IHit *>> &hitMap, int count ){
    /************************************************************/
    // FSI Hits
    int nfsi = 0;
    St_g2t_fts_hit *g2t_fsi_hits = (St_g2t_fts_hit *)GetDataSet("geant/g2t_fsi_hit");

    if (g2t_fsi_hits == nullptr) {
        LOG_INFO << "g2t_fsi_hits is null" << endm;
    } else {
        nfsi = g2t_fsi_hits->GetNRows();
    }

    // reuse this to store cov mat
    TMatrixDSym hitCov3(3);
    
    if ( mGenHistograms ) this->mHistograms["nHitsFSI"]->Fill(nfsi);
    LOG_INFO << "# fsi hits = " << nfsi << endm;

    for (int i = 0; i < nfsi; i++) {

        g2t_fts_hit_st *git = (g2t_fts_hit_st *)g2t_fsi_hits->At(i);
        
        if (0 == git)
            continue; // geant hit
        
        int track_id = git->track_p;
        int volume_id = git->volume_id;  // 4, 5, 6
        int d = volume_id / 1000;        // disk id
        int w = (volume_id % 1000) / 10; // wedge id
        int s = volume_id % 10;          // sensor id
        
        int plane_id = d - 4;
        float x = git->x[0];
        float y = git->x[1];
        float z = git->x[2];

        if (mSiRasterizer->active()) {
            TVector3 rastered = mSiRasterizer->raster(TVector3(git->x[0], git->x[1], git->x[2]));
            
            if ( mGenHistograms ) {
                this->mHistograms["fsiHitDeltaR"]->Fill(sqrt(x * x + y * y) - rastered.Perp());
                this->mHistograms["fsiHitDeltaPhi"]->Fill(atan2(y, x) - rastered.Phi());
            }
            x = rastered.X();
            y = rastered.Y();
        }


        if ( mGenHistograms ) this->mHistograms["fsi_volume_id"]->Fill(d);

        if (plane_id < 3 && plane_id >= 0) {

            if ( mGenHistograms ) {
                this->mHistograms[TString::Format("fsi%dHitMap", plane_id).Data()]->Fill(x, y);
                this->mHistograms[TString::Format("fsi%dHitMapR", plane_id).Data()]->Fill(sqrt(x * x + y * y));
                this->mHistograms[TString::Format("fsi%dHitMapPhi", plane_id).Data()]->Fill(atan2(y, x) + TMath::Pi());
            }
        } else {
            continue;
        }

        hitCov3 = makeSiCovMat( TVector3( x, y, z ), mFwdConfig );
        FwdHit *hit = new FwdHit(count++, x, y, z, d, track_id, hitCov3, mcTrackMap[track_id]);

        // Add the hit to the hit map
        hitMap[hit->getSector()].push_back(hit);
    }
} // loadFstHitsFromGEANT

void StFwdTrackMaker::loadMcTracks( std::map<int, std::shared_ptr<McTrack>> &mcTrackMap ){
    // Get geant tracks
    St_g2t_track *g2t_track = (St_g2t_track *)GetDataSet("geant/g2t_track");

    if (!g2t_track)
        return;

    mTreeNTracks = 1;
    LOG_INFO << g2t_track->GetNRows() << " mc tracks in geant/g2t_track " << endm;
    if ( mGenHistograms ) this->mHistograms["nMcTracks"]->Fill(g2t_track->GetNRows());

    for (int irow = 0; irow < g2t_track->GetNRows(); irow++) {
        g2t_track_st *track = (g2t_track_st *)g2t_track->At(irow);

        if (0 == track)
            continue;

        int track_id = track->id;
        float pt2 = track->p[0] * track->p[0] + track->p[1] * track->p[1];
        float pt = sqrt(pt2);
        float eta = track->eta;
        float phi = atan2(track->p[1], track->p[0]); //track->phi;
        int q = track->charge;

        if (0 == mcTrackMap[track_id] ) 
            mcTrackMap[track_id] = shared_ptr<McTrack>(new McTrack(pt, eta, phi, q, track->start_vertex_p));
        
        if (mGenTree) {
            // this is only turnd on for debug
            LOG_INFO << "mTreeNTracks = " << mTreeNTracks << " == track_id = " << track_id << " , is_shower = " << track->is_shower << ", start_vtx = " << track->start_vertex_p << endm;
            mTreePt[mTreeNTracks] = pt;
            mTreeEta[mTreeNTracks] = eta;
            mTreePhi[mTreeNTracks] = phi;
            mTreeNTracks++;
        }

    } // loop on track (irow)
} // loadMcTracks


//________________________________________________________________________
int StFwdTrackMaker::Make() {

    long long itStart = FwdTrackerUtils::nowNanoSecond();
    
    std::map<int, shared_ptr<McTrack>> &mcTrackMap = mForwardHitLoader->_mctracks;
    std::map<int, std::vector<KiTrack::IHit *>> &hitMap = mForwardHitLoader->_hits;
    std::map<int, std::vector<KiTrack::IHit *>> &fsiHitMap = mForwardHitLoader->_fsi_hits;

    loadMcTracks( mcTrackMap );

    
    // now check the Mc tracks against the McEvent filter
    size_t nForwardTracks = 0;
    size_t nForwardTracksNoThreshold = 0;
    for (auto mctm : mcTrackMap ){
        if ( mctm.second == nullptr ) continue;

        if ( mGenHistograms ){
            mHistograms[ "McEventPt" ] ->Fill( mctm.second->mPt );
            mHistograms[ "McEventEta" ] ->Fill( mctm.second->mEta );
            mHistograms[ "McEventPhi" ] ->Fill( mctm.second->mPhi );
        }

        if ( mctm.second->mEta > 2.5 && mctm.second->mEta < 4.0 ){
            
            if ( mGenHistograms ){
                mHistograms[ "McEventFwdPt" ] ->Fill( mctm.second->mPt );
                mHistograms[ "McEventFwdEta" ] ->Fill( mctm.second->mEta );
                mHistograms[ "McEventFwdPhi" ] ->Fill( mctm.second->mPhi );
            }

            nForwardTracksNoThreshold++;
            if ( mctm.second->mPt > 0.05  )
                nForwardTracks++;
        }
    } // loop on mcTrackMap

    if ( mGenHistograms ) {
        mHistograms[ "nMcTracksFwd" ]->Fill( nForwardTracks );
        mHistograms[ "nMcTracksFwdNoThreshold" ]->Fill( nForwardTracksNoThreshold );
    }

    size_t maxForwardTracks = mFwdConfig.get<size_t>( "McEvent.Mult:max", 10000 );
    if ( nForwardTracks > maxForwardTracks ){
        LOG_INFO << "Skipping event with more than " << maxForwardTracks << " forward tracks" << endm;
        return kStOk;
    }

    if ( IAttr("useFtt") ) 
        loadStgcHits( mcTrackMap, hitMap );
    
    if ( IAttr("useFst") )
        loadFstHits( mcTrackMap, fsiHitMap );

    // Process single event
    mForwardTracker->doEvent();



    if (mGenTree) {

        if (mForwardTracker->getSaveCriteriaValues()) {
            for (auto crit : mForwardTracker->getTwoHitCriteria()) {
                string name = crit->getName();
                mTreeCrits[name].clear();
                mTreeCritTrackIds[name].clear();
                // copy by value so ROOT doesnt get lost (uses pointer to vector)
                for (float v : mForwardTracker->getCriteriaValues(name)) {
                    mTreeCrits[name].push_back(v);
                }
                for (int v : mForwardTracker->getCriteriaTrackIds(name)) {
                    mTreeCritTrackIds[name].push_back(v);
                }
            }

            // three hit criteria
            for (auto crit : mForwardTracker->getThreeHitCriteria()) {
                string name = crit->getName();
                mTreeCrits[name].clear();
                mTreeCritTrackIds[name].clear();
                // copy by value so ROOT doesnt get lost (uses pointer to vector)
                for (float v : mForwardTracker->getCriteriaValues(name)) {
                    mTreeCrits[name].push_back(v);
                }
                for (int v : mForwardTracker->getCriteriaTrackIds(name)) {
                    mTreeCritTrackIds[name].push_back(v);
                }
            }
        }

        mTree->Fill();
    } // if mGenTree

    LOG_INFO << "Forward tracking on this event took " << (FwdTrackerUtils::nowNanoSecond() - itStart) * 1e-6 << " ms" << endm;


    StEvent *stEvent = static_cast<StEvent *>(GetInputDS("StEvent"));

    if ( IAttr("fillEvent") ) {

        if (!stEvent) {
            LOG_WARN << "No StEvent found. Forward tracks will not be saved" << endm;
            return kStWarn;
        }

        // Now fill StEvent
        FillEvent();

        // Now loop over the tracks and do printout
        int nnodes = stEvent->trackNodes().size();

        for ( int i = 0; i < nnodes; i++ ) {

            const StTrackNode *node = stEvent->trackNodes()[i];
            StGlobalTrack *track = (StGlobalTrack *)node->track(global);
            StTrackGeometry *geometry = track->geometry();

            StThreeVectorF origin = geometry->origin();
            StThreeVectorF momentum = geometry->momentum();


            StDcaGeometry *dca = track->dcaGeometry();
            if ( dca ) {
                origin = dca->origin();
                momentum = dca->momentum();
            }
            else {
                LOG_INFO << "d c a geometry missing" << endm;
            }

            int idtruth = track->idTruth();
            
            auto mctrack = mcTrackMap[ idtruth ];

        } // loop on nnodes

    } // IAttr FillEvent

    // delete the hits from the hitmap
    for ( auto kv : hitMap ){
        for ( auto h : kv.second ){
            delete h;
        }
        kv.second.clear();
    }

    for ( auto kv : fsiHitMap ){
        for ( auto h : kv.second ){
            delete h;
        }
        kv.second.clear();
    }
    

    return kStOK;
} // Make
//________________________________________________________________________
void StFwdTrackMaker::Clear(const Option_t *opts) {
    mForwardHitLoader->clear();
}
//________________________________________________________________________

void StFwdTrackMaker::FillEvent()
{
    StEvent *stEvent = static_cast<StEvent *>(GetInputDS("StEvent"));

    // Track seeds
    const auto &seed_tracks = mForwardTracker -> getRecoTracks();
    // Reconstructed globals
    const auto &genfitTracks = mForwardTracker -> globalTracks();

    // Clear up somethings... (but does this interfere w/ Sti and/or Stv?)
    StEventHelper::Remove(stEvent, "StSPtrVecTrackNode");
    StEventHelper::Remove(stEvent, "StSPtrVecPrimaryVertex");

    // StiStEventFiller fills track nodes and detector infos by reference... there
    // has got to be a cleaner way to do this, but for now follow along.
    auto &trackNodes         = stEvent->trackNodes();
    auto &trackDetectorInfos = stEvent->trackDetectorInfo();

    int track_count_total  = 0;
    int track_count_accept = 0;

    for ( auto *genfitTrack : genfitTracks ) {

        // Get the track seed
        const auto &seed = seed_tracks[track_count_total];

        // Increment total track count
        track_count_total++;

        // Check to see if the track passes cuts (it should, for now)
        if ( 0 == accept(genfitTrack) ) continue;

        track_count_accept++;

        // Create a detector info object to be filled
        StTrackDetectorInfo *detectorInfo = new StTrackDetectorInfo;
        FillDetectorInfo( detectorInfo, genfitTrack, true );

        // Create a new track node (on which we hang a global and, maybe, primary track)
        StTrackNode *trackNode = new StTrackNode;

        // This is our global track, to be filled from the genfit::Track object "track"
        StGlobalTrack *globalTrack = new StGlobalTrack;

        // Fill the track with the good stuff
        FillTrack( globalTrack, genfitTrack, seed, detectorInfo );
        FillTrackDcaGeometry( globalTrack, genfitTrack );
        trackNode->addTrack( globalTrack );

        // On successful fill (and I don't see why we wouldn't be) add detector info to the list
        trackDetectorInfos.push_back( detectorInfo );

        trackNodes.push_back( trackNode );

        // // Set relationships w/ tracker object and MC truth
        // // globalTrack->setKey( key );
        // // globalTrack->setIdTruth( idtruth, qatruth ); // StTrack is dominant contributor model

        // // Add the track to its track node
        // trackNode->addTrack( globalTrack );
        // trackNodes.push_back( trackNode );

        // NOTE: could we qcall here mForwardTracker->fitTrack( seed, vertex ) ?

    } // end of loop over tracks

    LOG_INFO << "  number visited  = " <<   track_count_total  << endm;
    LOG_INFO << "  number accepted = " <<   track_count_accept << endm;
}


void StFwdTrackMaker::FillTrack( StTrack *otrack, const genfit::Track *itrack, const Seed_t &iseed, StTrackDetectorInfo *info )
{
    vector<double> fttZ;
    if ( gGeoManager ){
        FwdGeomUtils fwdGeoUtils( gGeoManager );
        fttZ = fwdGeoUtils.fttZ( {0.0, 0.0, 0.0, 0.0} );
    } else {
        fttZ = {0.0, 0.0, 0.0, 0.0};
        LOG_WARN << "Could not load Ftt geometry, tracks will be invalid" << endm;
    }

    // otrack == output track
    // itrack == input track (genfit)

    otrack->setEncodedMethod(kUndefinedFitterId);

    // Track length and TOF between first and last point on the track
    // TODO: is this same definition used in StEvent?
    double track_len = itrack->getTrackLen();


    //  double track_tof = itrack->getTrackTOF();
    otrack->setLength( abs(track_len) );

    // Get the so called track seed quality... the number of hits in the seed
    int seed_qual = iseed.size();
    otrack->setSeedQuality( seed_qual );

    // Set number of possible points in each detector
    // TODO: calcuate the number of possible points in each detector, for now set = 4
    otrack->setNumberOfPossiblePoints( 4, kUnknownId );


    // Calculate TheTruth from the track seed for now.   This will be fine as
    // long as we do not "refit" the track, potentially removing original seed
    // hits from the final reconstructed track.

    // Apply dominant contributor model to the track seed
    int idtruth, qatruth;
    idtruth = TheTruth( iseed, qatruth );

    otrack->setIdTruth( idtruth, qatruth ); // StTrack is dominant contributor model


    // Fill the inner and outer geometries of the track.  For now,
    // always propagate the track to the first layer of the silicon
    // to fill the inner geometry.
    //
    // TODO: We may need to extend our "geometry" classes for RK parameters
    FillTrackGeometry( otrack, itrack, fttZ[0], kInnerGeometry );
    FillTrackGeometry( otrack, itrack, fttZ[3], kOuterGeometry );

    // Next fill the fit traits
    FillTrackFitTraits( otrack, itrack );

    // Set detector info
    otrack->setDetectorInfo( info );

    // NOTE:  StStiEventFiller calls StuFixTopoMap here...

    // Fill the track flags
    FillTrackFlags( otrack, itrack );

    //covM[k++] = M(0,5); covM[k++] = M(1,5); covM[k++] = M(2,5); covM[k++] = M(3,5); covM[k++] = M(4,5); covM[k++] = M(5,5);
}


void StFwdTrackMaker::FillTrackFlags( StTrack *otrack, const genfit::Track *itrack )
{

    int flag = 0;
    // StiStEventFiller::setFlag does two things.  1) it sets the track flags, indicating
    // which detectors have participated in the track.  It is a four digit value encoded
    // as follows (from StTrack.h):

    /* --------------------------------------------------------------------------
    *  The track flag (mFlag accessed via flag() method) definitions with ITTF
    *  (flag definition in EGR era can be found at
    *   http://www.star.bnl.gov/STAR/html/all_l/html/dst_track_flags.html)
    *
    *  mFlag= zxyy, where  z = 1 for pile up track in TPC (otherwise 0)
    *                      x indicates the detectors included in the fit and
    *                     yy indicates the status of the fit.
    *  Positive mFlag values are good fits, negative values are bad fits.
    *
    *  The first digit indicates which detectors were used in the refit:
    *
    *      x=1 -> TPC only
    *      x=3 -> TPC       + primary vertex
    *      x=5 -> SVT + TPC
    *      x=6 -> SVT + TPC + primary vertex
    *      x=7 -> FTPC only
    *      x=8 -> FTPC      + primary
    *      x=9 -> TPC beam background tracks
    *
    *  The last two digits indicate the status of the refit:
    *       = +x01 -> good track
    *
    *      = -x01 -> Bad fit, outlier removal eliminated too many points
    *      = -x02 -> Bad fit, not enough points to fit
    *      = -x03 -> Bad fit, too many fit iterations
    *      = -x04 -> Bad Fit, too many outlier removal iterations
    *      = -x06 -> Bad fit, outlier could not be identified
    *      = -x10 -> Bad fit, not enough points to start
    *
    *      = +x11 -> Short track pointing to EEMC */

    // NOTE: First digit will be used as follows for forward tracks
    //
    // x = 5 sTGC only
    // x = 6 sTGC + primary vertex
    // x = 7 sTGC + forward silicon
    // x = 8 sTGC + forward silicon + primary vertex

    if      ( global  == otrack->type() ) flag = 501;
    else if ( primary == otrack->type() ) flag = 601;

    // TODO: detect presence of silicon hits and add appropriately to the flag


    // As for "bad" fits, I believe GenFit does not propagate fit information for
    // failed fits.  (???).  So we will not publish bad track flags.
    otrack->setFlag(flag);
}


void StFwdTrackMaker::FillTrackMatches( StTrack *otrack, const genfit::Track *itrack )
{
    // TODO:

    // At midrapidity, we extend the track to the fast detectors and check to see whether
    // the track matches an active element or not.  The fast detectors are the barrel time-
    // of-flight, the barrel EM calorimeter and teh endcap EM calorimeter.

    // We will be interested in matching FTS tracks to the following subsystems:
    // 1) The event plane detector
    // 2) Forward EM cal
    // 3) Forward Hadronic cal

    // We could adopt the following scheme to save the track fit information in a way that
    // can be accessed later, without modification to the StEvent data model...

    // Save the state of the fit (mapped to a helix) at the first silicon layer as the inner geometry.
    // Save the state of the fit (mapped to a helix) at the event plane detector as the outer geometry.
    // Save the state of the fit (mapped to a helix) at the front of the EM cal as the "Ext" geometry
    // ... helix would have no curvature at that point and would be a straight line, as there is no b field.
    // ... can easily get to the HCAL from there...
}


void StFwdTrackMaker::FillTrackFitTraits( StTrack *otrack, const genfit::Track *itrack )
{

    unsigned short g3id_pid_hypothesis = 6; // TODO: do not hard code this

    // Set the chi2 of the fit.  The second element in the array is the incremental
    // chi2 for adding the vertex to the primary track.
    float chi2[] = {0, -999};
    const auto *fit_status = itrack->getFitStatus();

    if ( 0 == fit_status ) {
        LOG_WARN << "genfit track with no fit status" << endm;
        return;
    }

    chi2[0] = fit_status->getChi2();
    int ndf = fit_status->getNdf();

    chi2[0] /= ndf; // TODO: Check if this is right

    // ... odd that we make this determination based on the output track's type ...
    if ( primary == otrack->type() ) {
        // TODO: chi2[1] should hold the incremental chi2 of adding the vertex for the primary track
        //       is this available from genfit?
    }

    // Covariance matrix is next.  This one should be fun.  StEvent assumes the helix
    // model, but we have fit to the Runga Kutta track model.  The covariance matrix
    // is different.  So... TODO:  Do we need to specify covM for the equivalent helix?
    float covM[15] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };

    // Obtain fitted state so we can grab the covariance matrix
    genfit::MeasuredStateOnPlane state = itrack->getFittedState(0);

    // For global tracks, we are evaluating the fit at the first silicon plane.
    // Extrapolate the fit to this point so we can extract the covariance matrix
    // there.  For primary track, point 0 should correspond to the vertex.
    //
    // TODO: verify first point on primary tracks is the vertex.

    // Grab the covariance matrix
    const auto &M = state.getCov();

    // TODO: This is where we would do the math and transform from the Runga Kutta basis
    //       to the helix basis... but do we need to?

    int k = 0;
    covM[k++] = M(0, 0);
    covM[k++] = M(0, 1); covM[k++] = M(1, 1);
    covM[k++] = M(0, 2); covM[k++] = M(1, 2); covM[k++] = M(2, 2);
    covM[k++] = M(0, 3); covM[k++] = M(1, 3); covM[k++] = M(2, 3); covM[k++] = M(3, 3);
    covM[k++] = M(0, 4); covM[k++] = M(1, 4); covM[k++] = M(2, 4); covM[k++] = M(3, 4); covM[k++] = M(4, 4);

    StTrackFitTraits fit_traits(g3id_pid_hypothesis, 0, chi2, covM);

    // Get number of hits in all detectors
    int nhits[kMaxDetectorId] = {};

    for ( const auto *point : itrack->getPoints() ) {

        const auto *measurement = point->getRawMeasurement();
        int detId = measurement->getDetId();
        nhits[detId]++;

    }

    for ( int i = 0; i < kMaxDetectorId; i++ ) {
        if ( 0 == nhits[i] ) continue; // not sure why, but Sti skips setting zero hits

        fit_traits.setNumberOfFitPoints( (unsigned char)nhits[i], (StDetectorId)i );
    }

    if ( primary == otrack->type() ) {
        fit_traits.setPrimaryVertexUsedInFit( true );
    }

    otrack -> setFitTraits( fit_traits );

}


void StFwdTrackMaker::FillTrackGeometry( StTrack *otrack, const genfit::Track *itrack, double zplane, int io )
{
    int ipoint = 0;

    if ( io == kInnerGeometry ) ipoint = 0; // hardcoded to sTGC only for now
    else                        ipoint = 3;

    // Obtain fitted state
    genfit::MeasuredStateOnPlane measuredState = itrack->getFittedState(ipoint);

    // Obtain the cardinal representation
    genfit::AbsTrackRep *cardinal = itrack->getCardinalRep();

    // We really don't want the overhead in the TVector3 ctor/dtor here
    static TVector3 xhat(1, 0, 0), yhat(0, 1, 0), Z(0, 0, 0);

    // Assign the z position
    Z[2] = zplane;

    // This is the plane for which we are evaluating the fit
    const auto detectorPlane = genfit::SharedPlanePtr( new genfit::DetPlane(Z, xhat, yhat) );

    // Update the state to the given plane
    try {
        cardinal->extrapolateToPlane( measuredState, detectorPlane, false, true );
    } catch ( genfit::Exception &e ) {
        LOG_WARN << e.what() << endm;
        LOG_WARN << "Extraploation to inner/outer geometry point failed" << endm;
        return;
    }


    static StThreeVector<double> momentum;
    static StThreeVector<double> origin;

    static TVector3 pos;
    static TVector3 mom;
    static TMatrixDSym cov;

    measuredState.getPosMomCov(pos, mom, cov);

    for ( int i = 0; i < 3; i++ ) momentum[i] = mom[i];
    for ( int i = 0; i < 3; i++ ) origin[i]   = pos[i];

    double charge = measuredState.getCharge();

    // Get magnetic field
    double X[] = { pos[0], pos[1], pos[2] };
    double B[] = { 0, 0, 0 };
    StarMagField::Instance()->Field( X, B );

    // This is really an approximation, should be good enough for the inner
    // geometry (in the Silicon) but terrible in the outer geometry ( sTGC)
    double Bz = B[2];

    // Temporary helix to get the helix parameters
    StPhysicalHelix helix( momentum, origin, Bz*units::kilogauss, charge );
    // StiStEventFiller has this as |curv|.
    double curv = TMath::Abs(helix.curvature());
    double h    = -TMath::Sign(charge * Bz, 1.0); // helicity

    if ( charge == 0 ) h = 1;

    //
    // From StHelix::helix()
    //
    // phase = mPsi - h*pi/2
    // so...
    // psi = phase + h*pi/2
    //
    double psi = helix.phase() + h * TMath::Pi() / 2;
    double dip  = helix.dipAngle();
    short  q    = charge; assert( q == 1 || q == -1 || q == 0 );

    // Create the track geometry
    StTrackGeometry *geometry = new StHelixModel (q, psi, curv, dip, origin, momentum, h);

    if ( kInnerGeometry == io ) otrack->setGeometry( geometry );
    else                        otrack->setOuterGeometry( geometry );
}


void StFwdTrackMaker::FillTrackDcaGeometry( StGlobalTrack *otrack, const genfit::Track *itrack )
{
    // We will need the event
    StEvent *stEvent = static_cast<StEvent *>(GetInputDS("StEvent"));
    assert(stEvent); // we warned ya

    // And the primary vertex
    const StPrimaryVertex* primaryVertex = stEvent->primaryVertex(0);

    // Obtain fitted state from genfit track
    genfit::MeasuredStateOnPlane measuredState = itrack->getFittedState(1);

    // Obtain the cardinal representation
    genfit::AbsTrackRep *cardinal =  itrack->getCardinalRep();

    static TVector3 vertex;
    double x = 0;
    double y = 0;
    double z = 0;
    if ( primaryVertex ) {
        x = primaryVertex->position()[0];
        y = primaryVertex->position()[1];
        z = primaryVertex->position()[2];
    }
    vertex.SetX(x); vertex.SetY(y); vertex.SetZ(z);

    const static TVector3 direct(0., 0., 1.); // TODO get actual beamline slope

    // Extrapolate the measured state to the DCA of the beamline
    try {
        cardinal->extrapolateToLine(  measuredState, vertex, direct, false, true );
    }catch ( genfit::Exception &e ) {
        LOG_WARN << e.what() << "\n"
                    << "Extrapolation to beamline (DCA) failed." << "\n"
                    << "... vertex " << x << " " << y << "  " << z << endm;
        return;
    }

    static StThreeVector<double> momentum;
    static StThreeVector<double> origin;

    //
    // These lines obtain the position, momentum and covariance matrix for the fit
    //

    static TVector3 pos;
    static TVector3 mom;

    measuredState.getPosMom(pos, mom);

    for ( int i = 0; i < 3; i++ ) momentum[i] = mom[i];
    for ( int i = 0; i < 3; i++ ) origin[i]   = pos[i];

    double charge = measuredState.getCharge();

    static TVectorD    state(5);
    static TMatrixDSym cov(5);

    //
    //  this is the 5D state and covariance matrix
    //  https://arxiv.org/pdf/1902.04405.pdf
    //  state = { q/p, u', v', u, v }, where
    //  q/p is charge over momentum
    //  u,  v correspond to x, y (I believe)
    //  u', v' are the direction cosines with respect to the plane
    //  ... presume that
    //      u' = cos(thetaX)
    //      v' = cos(thetaY)
    //

    state = measuredState.getState();
    cov   = measuredState.getCov();

    // Below is one way to convert the parameters to a helix, using the
    // StPhysicalHelix class

    double pt     = momentum.perp();
    double ptinv  = (pt != 0) ? 1.0 / pt : std::numeric_limits<double>::max();

    // Get magnetic field
    double X[] = { pos[0], pos[1], pos[2] };
    double B[] = { 0, 0, 0 };
    StarMagField::Instance()->Field( X, B );

    // This is really an approximation, should be good enough for the inner
    // geometry (in the Silicon) but terrible in the outer geometry ( sTGC)
    double Bz = B[2];

    // Temporary helix to get the helix parameters
    StPhysicalHelix helix( momentum, origin, Bz*units::kilogauss, charge );

    // StiStEventFiller has this as |curv|.
    double curv =  TMath::Abs(helix.curvature());
    double h    = -TMath::Sign(charge * Bz, 1.0); // helicity

    if ( charge == 0 ) h = 1;

    //
    // From StHelix::helix()
    //
    // phase = mPsi - h*pi/2
    // so...
    // psi = phase + h*pi/2
    //
    double psi  = helix.phase() + h * TMath::Pi() / 2;
    double dip  = helix.dipAngle();
    double tanl = TMath::Tan(dip); 
    short  q    = charge; assert( q == 1 || q == -1 || q == 0 );

    // TODO:verify this and investigate numerical method for errors
    double mImp  = origin.perp();
    double mZ    = origin[2];
    double mPsi  = psi;
    double mPti  = ptinv;
    double mTan  = tanl;
    double mCurv = curv;

    double p[] = { mImp, mZ, mPsi, mPti, mTan, mCurv };

    // TODO: fill in errors... (do this numerically?)
    double e[15] = {};

    StDcaGeometry *dca = new StDcaGeometry;
    otrack->setDcaGeometry(dca);
    dca->set(p, e);
}


void StFwdTrackMaker::FillDetectorInfo( StTrackDetectorInfo *info, const genfit::Track *track, bool increment )
{
    //   // here is where we would fill in
    //   // 1) total number of hits
    //   // 2) number of sTGC hits
    //   // 3) number of silicon hits
    //   // 4) an StHit for each hit fit to the track
    //   // 5) The position of the first and last hits on the track

    int ntotal   = track->getNumPoints(); // vs getNumPointsWithMeasurement() ?

    StThreeVectorF firstPoint(0, 0, 9E9);
    StThreeVectorF lastPoint(0, 0, -9E9);

    int count = 0;

    for ( const auto *point : track->getPoints() ) {

    const auto *measurement = point->getRawMeasurement();
    if ( !measurement ) {
        continue;
    }

    const TVectorD &xyz = measurement->getRawHitCoords();
    float x = xyz[0];
    float y = xyz[1];
    float z = 0; // We get this from the detector plane...

    // Get fitter info for the cardinal representation
    const auto *fitinfo = point->getFitterInfo();
    if ( !fitinfo ) {
        continue;
    }

    const auto &plane = fitinfo->getPlane();
    TVector3 normal = plane->getNormal();
    const TVector3 &origin = plane->getO();

    z = origin[2];

    if ( z > lastPoint[2] ) {
        lastPoint.setX(x);      lastPoint.setY(y);      lastPoint.setZ(z);
    }

    if ( z < firstPoint[2] ) {
        firstPoint.setX(x);     firstPoint.setY(y);     firstPoint.setZ(z);
    }

    ++count;

    //We should also convert (or access) StHit and add to the track detector info

    }

    info->setNumberOfPoints( (unsigned char)count, kUnknownId ); // TODO: Assign after StEvent is updated


    assert(count);

    info->setFirstPoint( firstPoint );
    info->setLastPoint( lastPoint );
    info->setNumberOfPoints( ntotal, kUnknownId ); // TODO: Assign after StEvent is updated
}
