#ifndef FWD_GEOM_UTILS_H
#define FWD_GEOM_UTILS_H

#include "TGeoVolume.h"
#include "TGeoNode.h"
#include "TGeoMatrix.h"
#include "TGeoNavigator.h"

class FwdGeomUtils {
    public:

        FwdGeomUtils( TGeoManager * gMan ) {
            if ( gMan != nullptr )
                _navigator = gMan->AddNavigator();
        }

        ~FwdGeomUtils(){

        }

        bool cd( const char* path ){
            // Change to the specified path
            bool ret = _navigator -> cd(path);
            // If successful, set the node, the volume, and the GLOBAL transformation
            // for the requested node.  Otherwise, invalidate these
            if ( ret ) {
                _matrix = _navigator->GetCurrentMatrix();
                _node   = _navigator->GetCurrentNode();
                _volume = _node->GetVolume();
            } else {
                _matrix = 0; _node = 0; _volume = 0;
            }
            return ret;
        }

        vector<double> fttZ( vector<double> defaultZ ) {
            double z0 = fttZ(0);
            if ( z0 > 1.0 ) { // returns 0 on faiure
                vector<double> z = {z0, fttZ(1), fttZ(2), fttZ(3)};
            }
            return defaultZ;
        }
        double fttZ( int index ) {

            // This ftt_z_delta is needed to match the z location of hits (midpint of active volume?) to the z location of the mother volume.  
            // NOTE: It may be possible to improve this when the higher precision FTT geometry model is added
            const double ftt_z_delta = 0.435028;
            stringstream spath;
            spath << "/HALL_1/CAVE_1/STGM_1/TGCP_" << (index + 1) * 8 << "/"; 
            // 0 -> 8
            // 1 -> 16
            // 2 -> 24
            // 3 -> 32
            bool can = cd( spath.str().c_str() );
            if ( can && _matrix != nullptr ){
                return _matrix->GetTranslation()[2] + ftt_z_delta;
            }
            return 0.0;
        }

        vector<double> fstZ( vector<double> defaultZ ) {
            double z0 = fstZ(0);
            if ( z0 > 1.0 ) { // returns 0 on faiure
                vector<double> z = {z0, fstZ(1), fstZ(2)};
            }
            return defaultZ;
        }

        double fstZ( int index ) {
            stringstream spath;
            spath << "/HALL_1/CAVE_1/FTSM_1/FTSD_" << (index + 1) << "/"; 
            bool can = cd( spath.str().c_str() );
            if ( can && _matrix != nullptr ){
                return _matrix->GetTranslation()[2];
            }
            return 0.0;
        }

    protected:
    TGeoVolume    *_volume    = nullptr;
    TGeoNode      *_node      = nullptr;
    TGeoHMatrix   *_matrix    = nullptr;
    TGeoIterator  *_iter      = nullptr;
    TGeoNavigator *_navigator = nullptr;
};

#endif