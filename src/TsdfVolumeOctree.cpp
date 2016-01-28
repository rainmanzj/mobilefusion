#include "TsdfVolumeOctree.h"

#include <limits>
#include <iostream>
#include <fstream>

#include <pcl/console/time.h>
#include <pcl/common/io.h>

using std::vector;
using std::string;
using std::endl;

namespace MobileFusion {
    TSDFVolumeOctree::TSDFVolumeOctree()
        : xres_ (128)
        , yres_ (128)
        , zres_ (128)
        , xsize_ (1.0f)
        , ysize_ (1.0f)
        , zsize_ (1.0f)
        , max_dist_pos_ (0.03f)
        , max_dist_neg_ (0.03f)
        , max_weight_ (50)
        , min_sensor_dist_ (0.3f)
        , max_sensor_dist_ (3.0f)
        , focal_length_x_ (540.686f)
        , focal_length_y_ (540.686f)
        , principal_point_x_ (256.0f)
        , principal_point_y_ (212.0f)
        , image_width_ (512)
        , image_height_ (424)
        , max_cell_size_x_ (0.25f)
        , max_cell_size_y_ (0.25f)
        , max_cell_size_z_ (0.25f)
        , UNOBSERVED_VOXEL (std::numeric_limits<float>::quiet_NaN ())
        , weight_by_depth_ (false)
        , weight_by_variance_ (false)
        , integrate_color_ (true)
        , color_mode_ ("RGB")
        , use_trilinear_interpolation_ (false)
        , num_random_splits_ (1) {
              global_transform_ = Eigen::Affine3d::Identity();
        }

    TSDFVolumeOctree::~TSDFVolumeOctree () {
    }

    void TSDFVolumeOctree::setGlobalTransform ( const Eigen::Affine3d &trans) {
        global_transform_ = trans;
    }

    void TSDFVolumeOctree::setResolution (int xres, int yres, int zres) {
        xres_ = xres;
        yres_ = yres;
        zres_ = zres;
    }

    void TSDFVolumeOctree::getResolution (int &xres, int &yres, int &zres) const {
        xres = xres_;
        yres = yres_;
        zres = zres_;
    }

    void TSDFVolumeOctree::setGridSize (float xsize, float ysize, float zsize) {
        xsize_ = xsize;
        ysize_ = ysize;
        zsize_ = zsize;
    }

    void TSDFVolumeOctree::getGridSize (float &xsize, float &ysize, float &zsize) const {
        xsize = xsize_;
        ysize = ysize_;
        zsize = zsize_;
    }

    void TSDFVolumeOctree::setImageSize (int width, int height) {
        image_width_ = width;
        image_height_ = height;
    }

    void TSDFVolumeOctree::getImageSize (int &width, int &height) const {
        width = image_width_;
        height = image_height_;
    }

    void TSDFVolumeOctree::setDepthTruncationLimits (float max_dist_pos, float max_dist_neg) {
        max_dist_pos_ = max_dist_pos;
        max_dist_neg_ = max_dist_neg;
    }

    void TSDFVolumeOctree::getDepthTruncationLimits (float &max_dist_pos, float &max_dist_neg) const {
        max_dist_pos = max_dist_pos_;
        max_dist_neg = max_dist_neg_;
    }

    void TSDFVolumeOctree::setWeightTruncationLimit (float max_weight) {
        max_weight_ = max_weight;
    }

    float TSDFVolumeOctree::getWeightTruncationLimit () const {
        return (max_weight_);
    }

    void TSDFVolumeOctree::setCameraIntrinsics (const double focal_length_x,
            const double focal_length_y,
            const double principal_point_x,
            const double principal_point_y) {
        focal_length_x_ = focal_length_x;
        focal_length_y_ = focal_length_y;
        principal_point_x_ = principal_point_x;
        principal_point_y_ = principal_point_y;
    }

    void TSDFVolumeOctree::getCameraIntrinsics (double &focal_length_x,
            double &focal_length_y,
            double &principal_point_x,
            double &principal_point_y) const {
        focal_length_x = focal_length_x_;
        focal_length_y = focal_length_y_;
        principal_point_x = principal_point_x_;
        principal_point_y = principal_point_y_;
    }

    void TSDFVolumeOctree::reset () {
        if (integrate_color_)
            octree_.reset (new Octree (xres_, yres_, zres_, xsize_, ysize_, zsize_, color_mode_));
        else
            octree_.reset (new Octree (xres_, yres_, zres_, xsize_, ysize_, zsize_));

        octree_->init (max_cell_size_x_, max_cell_size_y_, max_cell_size_z_);
        std::vector<OctreeNode::Ptr> leaves;
        octree_->getLeaves (leaves);
#pragma omp parallel for
        for (size_t i = 0; i < leaves.size (); ++i) {
            leaves[i] -> setData (-1, 0);
        }
    }

    pcl::PointCloud<pcl::PointNormal>::Ptr TSDFVolumeOctree::renderView (const Eigen::Affine3d &trans, int downsampleBy) const {
        int new_width = image_width_ / downsampleBy;
        int new_height = image_height_ / downsampleBy;
        double new_fx = focal_length_x_ / downsampleBy;
        double new_fy = focal_length_y_ / downsampleBy;
        double new_cx = principal_point_x_ / downsampleBy;
        double new_cy = principal_point_y_ / downsampleBy;
        pcl::PointCloud<pcl::PointNormal>::Ptr cloud (new pcl::PointCloud<pcl::PointNormal> (new_width, new_height));
        cloud->is_dense = false;

        float min_step = max_dist_neg_ * 3/4. ;
//#pragma omp parallel for
        for (size_t i = 0; i < cloud->size(); ++i) {
            size_t x = i % new_width;
            size_t y = i / new_width;

            pcl::PointNormal &pt = cloud->operator() (x,y);
            bool found_crossing = false;
            Eigen::Vector3f du ( (x - new_cx)/new_fx,
                                 (y - new_cy)/new_fy,
                                  1);
            du.normalize();
            //Apply transform -- rotate the ray, start at the offset
            du = trans.rotation().cast<float>() * du;
            Eigen::Vector3f p = trans.translation().cast<float>();
            //preallocate
            int x_i, y_i, z_i;
            float d;
            float w;
            float last_w = 0;
            float last_d = 0;
            float t = min_sensor_dist_;
            p += t*du;
            float step = min_step;
            //check if ray intersects cube
            //Initially we haven't ever hit a voxel
            bool hit_voxel = false;
            int niter = 0;
            while (t < max_sensor_dist_ ) {
                const OctreeNode* voxel = octree_->getContainingVoxel (p (0), p (1), p(2));
                if (voxel) {
                    hit_voxel = true;
                    voxel->getData(d,w);
                    if (((d < 0 && last_d > 0) || (d > 0 && last_d < 0 )) && last_w && w) {
                        found_crossing = true;
                        float old_t = t - step;
                        step = (zsize_/zres_)/2.; //Refine to do binary search
                        float new_d;
                        float new_w;
                        float last_new_d = d;
                        float last_new_w = w;
                        while (t >= old_t) {
                            t -= step;
                            p -= step*du;
                            voxel = octree_->getContainingVoxel (p (0), p(1), p(2));
                            if(!voxel)
                                break;
                            voxel->getData (new_d, new_w);
                            if((last_d > 0 && new_d > 0) || (last_d < 0 && new_d < 0)) {
                                last_d = new_d;
                                last_w = new_w;
                                d = last_new_d;
                                w = last_new_w;
                                t += step;
                                p += step*du;
                                break;
                            }
                            last_new_d = d;
                            last_new_w = w;
                        }
                        break;
                    }
                    last_d = d;
                    last_w = w;
                    //Update step
                    step = std::max (voxel->getMinSize() / 4., fabs (d) * max_dist_neg_);
                }
                else {
                    if (hit_voxel)
                        break;
                }
                t += step;
                p += step*du;
                niter++;
            }

            if (!found_crossing) {
                pt.x = pt.y = pt.z = std::numeric_limits<float>::quiet_NaN ();
            }

            else {
                //Get optimal t
                bool has_data = true;
                float tcurr = t;
                float tprev = t-step;
                last_d = getTSDFValue (trans.translation().cast<float> () + (tprev) * du, &has_data);
                d = getTSDFValue (trans.translation().cast<float> () + tcurr * du, &has_data);
                if(!has_data || pcl_isnan(d) || pcl_isnan (last_d)) {
                    pt.x = pt.y =pt.z = std::numeric_limits<float>::quiet_NaN ();
                }
                float t_star = t + step * (-1 + fabs (last_d / (last_d - d)));
                pt.getVector3fMap() = trans.translation().cast<float>() + t_star *du;
                //get normals by looking at adjacent voxels
                const OctreeNode* voxel = octree_->getContainingVoxel (pt.x, pt.y, pt.z);
                if(!voxel) {
                    pt.normal_x = pt.normal_y = pt.normal_z = std::numeric_limits<float>::quiet_NaN();
                    continue;
                }
                float xsize, ysize, zsize;
                voxel->getSize (xsize, ysize, zsize);
                bool valid = true;
                float d_xm = getTSDFValue (pt.x-xsize, pt.y, pt.z, &valid);
                float d_xp = getTSDFValue (pt.x+xsize, pt.y, pt.z, &valid);
                float d_ym = getTSDFValue (pt.x, pt.y-ysize, pt.z, &valid);
                float d_yp = getTSDFValue (pt.x, pt.y+ysize, pt.z, &valid);
                float d_zm = getTSDFValue (pt.x, pt.y, pt.z-zsize, &valid);
                float d_zp = getTSDFValue (pt.x, pt.y, pt.z+zsize, &valid);
                if(!valid) {
                    pt.normal_x = pt.normal_y = pt.normal_z = std::numeric_limits<float>::quiet_NaN ();
                    continue;
                }
                Eigen::Vector3f dF;
                dF (0) = (d_xp - d_xm) * max_dist_neg_ / (2*xsize);
                dF (1) = (d_yp - d_ym) * max_dist_neg_ / (2*ysize);
                dF (2) = (d_zp - d_zm) * max_dist_neg_ / (2*zsize);
                dF.normalize ();
                pt.normal_x = dF(0);
                pt.normal_y = dF(1);
                pt.normal_z = dF(2);
            }
        }
        pcl::transformPointCloudWithNormals (*cloud, *cloud, trans.inverse());
        return (cloud);
    }

    pcl::PointCloud<pcl::PointXYZRGBNormal>::Ptr TSDFVolumeOctree::renderColorView (const Eigen::Affine3d &trans, int downsampleBy) const {
        if (!integrate_color_)
            PCL_WARN ("[TSDFVolumeOCtree::renderColoredView] Rendering a colored view, but integrate_color_was not set!\n");
        pcl::PointCloud<pcl::PointNormal>::Ptr grayscale = renderView (trans, downsampleBy);
        pcl::PointCloud<pcl::PointXYZRGBNormal>::Ptr colored (new pcl::PointCloud<pcl::PointXYZRGBNormal> (grayscale->width, grayscale->height));
        colored->is_dense = false;
//#pragma omp parallel for
        for (size_t i = 0; i < colored->size(); i++) {
                pcl::PointXYZRGBNormal &pt = colored->at (i);
                pt.getVector3fMap() = grayscale->at(i).getVector3fMap();
                pt.getNormalVector3fMap() = grayscale->at(i).getNormalVector3fMap();
                if (pcl_isnan (pt.z))
                continue;
                Eigen::Vector3f v_t = trans.cast<float> () * pt.getVector3fMap ();
                const OctreeNode* voxel = octree_->getContainingVoxel (v_t (0), v_t (1), v_t(2));
                if (!voxel)
                continue;
                voxel->getRGB (pt.r, pt.g, pt.b);
                }
                return (colored);
        }

    float TSDFVolumeOctree::getTSDFValue (const Eigen::Vector3f &pt, bool* valid) const {
                return (getTSDFValue (pt (0), pt (1), pt(2)));
    }

    float TSDFVolumeOctree::getTSDFValue (float x, float y, float z, bool* valid) const {
        if (use_trilinear_interpolation_)
            return (interpolateTrilinearly (x, y, z, valid));
        else {
            const OctreeNode* voxel = octree_->getContainingVoxel (x, y, z);
                if (voxel != NULL && voxel->w_ > 0)
                    return (voxel->d_);
                else {
                    if (valid != NULL)
                        *valid = false;
                    return (std::numeric_limits<float>::quiet_NaN ());
                }
        }
    }

    float TSDFVolumeOctree::interpolateTrilinearly (const Eigen::Vector3f &pt, bool* valid) const {
        return (interpolateTrilinearly (pt (0), pt (1), pt (2), valid));
    }

    float TSDFVolumeOctree::interpolateTrilinearly (float x, float y, float z, bool* valid) const {
        int xi, yi, zi;
        bool exists = getVoxelIndex (x, y, z, xi, yi, zi);
        if (!exists || xi <= 0 || xi >= xres_-1 || yi <= 0 || yi >= yres_ -1 || zi <= 0 || zi >= zres_ -1) {
            if (valid != NULL) *valid = false;
            return (std::numeric_limits<float>::quiet_NaN ());
        }
        pcl::PointXYZ v = getVoxelCenter (xi, yi, zi);
        if (x < v.x) xi -= 1;
        if (y < v.y) yi -= 1;
        if (z < v.z) zi -= 1;
        v = getVoxelCenter (xi, yi, zi);
        pcl::PointXYZ vx =   getVoxelCenter (xi+1, yi, zi);
        pcl::PointXYZ vy =   getVoxelCenter (xi, yi+1, zi);
        pcl::PointXYZ vz =   getVoxelCenter (xi, yi, zi+1);
        pcl::PointXYZ vxy =  getVoxelCenter (xi+1, yi+1, zi);
        pcl::PointXYZ vxz =  getVoxelCenter (xi+1, yi, zi+1);
        pcl::PointXYZ vyz =  getVoxelCenter (xi, yi+1, zi+1);
        pcl::PointXYZ vxyz = getVoxelCenter (xi+1, yi+1, zi+1);

        float a = (x - v.x) * xres_ / xsize_;
        float b = (y - v.y) * yres_ / ysize_;
        float c = (z - v.z) * zres_ / zsize_;

        const OctreeNode * vo =   octree_->getContainingVoxel (v.x, v.y, v.z);
        const OctreeNode * vox =  octree_->getContainingVoxel (vx.x, vx.y, vx.z);
        const OctreeNode * voy =  octree_->getContainingVoxel (vy.x, vy.y, vy.z);
        const OctreeNode * voz =  octree_->getContainingVoxel (vz.x, vz.y, vz.z);

        const OctreeNode *voxy =  octree_->getContainingVoxel (vxy.x, vxy.y, vxy.z);
        const OctreeNode *voxz =  octree_->getContainingVoxel (vxz.x, vxz.y, vxz.z);
        const OctreeNode *voyz =  octree_->getContainingVoxel (vyz.x, vyz.y, vyz.z);

        const OctreeNode *voxyz = octree_->getContainingVoxel (vxyz.x, vxyz.y, vxyz.z);
        if (valid != NULL) {
            *valid &= (vo->w_ > 0);
            *valid &= (vox->w_ > 0);
            *valid &= (voy->w_ > 0);
            *valid &= (voz->w_ > 0);
            *valid &= (voxy->w_ > 0);
            *valid &= (voxz->w_ > 0);
            *valid &= (voyz->w_ > 0);
            *valid &= (voxyz->w_ > 0);
        }

        return ( vo->d_    * (1 - a) * (1 - b) * (1 - c) +
                 voz->d_   * (1 - a) * (1 - b) * (c) +
                 voy->d_   * (1 - a) * (b) * (1 - c) +
                 voyz->d_  * (1 - a) * (b) * (c) +
                 vox->d_   * (a) * (1 - b) * (1 - c) +
                 voxz->d_  * (a) * (1 - b) * (c) +
                 voxy->d_  * (a) * (b) * (1 - c) +
                 voxyz->d_ * (a) * (b) * (c));

    }

    pcl::PointCloud<pcl::Intensity>::Ptr TSDFVolumeOctree::getIntensityCloud (const Eigen::Affine3d &trans) const {
        return (pcl::PointCloud<pcl::Intensity>::Ptr ());
    }

    pcl::PointXYZ TSDFVolumeOctree::getVoxelCenter (size_t x, size_t y, size_t z) const {
        float xoff = xsize_/2;
        float yoff = ysize_/2;
        float zoff = zsize_/2;
        return pcl::PointXYZ (x*xsize_/xres_ - xoff, y*ysize_/yres_ - yoff, z*zsize_/zres_ - zoff);
    }

    bool TSDFVolumeOctree::getVoxelIndex (float x, float y, float z, int &x_i, int &y_i, int &z_i) const {
        double xoff = (double)xsize_/2;
        double yoff = (double)ysize_/2;
        double zoff = (double)zsize_/2;
        x_i = ((double)x + xoff) / (double)xsize_ * xres_;
        y_i = ((double)y + yoff) / (double)ysize_ * yres_;
        z_i = ((double)z + zoff) / (double)zsize_ * zres_;
        bool has_voxel = (x_i >= 0 && y_i >= 0 && z_i >= 0
                        && x_i < xres_ && y_i < yres_ && z_i < zres_);
        return (has_voxel);
    }

    pcl::PointCloud<pcl::PointXYZ>::ConstPtr TSDFVolumeOctree::getVoxelCenters (int nlevels) const {
        std::vector<OctreeNode::Ptr> voxels;
        octree_->getLeaves (voxels, nlevels);
        pcl::PointCloud<pcl::PointXYZ>::Ptr cloud (new pcl::PointCloud<pcl::PointXYZ> (voxels.size() ,1));
        for (size_t i = 0 ; i < voxels.size (); i++) {
            pcl::PointXYZ &pt = cloud->at(i);
            voxels[i]->getCenter (pt.x, pt.y, pt.z);
        }
        return (cloud);
    }

    void TSDFVolumeOctree::getOccupiedVoxelIndices (std::vector<Eigen::Vector3i> &indices) const {
        std::vector<OctreeNode::Ptr> leaves;
        octree_->getLeaves (leaves);
        float d;
        float w;
        float x, y ,z;
        for (size_t i = 0; i < leaves.size(); i++) {
            leaves[i]->getData (d,w);
            if (w > 0 && fabs (d) < 1) {
                leaves[i]->getCenter (x, y, z);
                Eigen::Vector3i idx;
                getVoxelIndex (x, y, z, idx(0), idx(1), idx(2));
                indices.push_back(idx);
            }
        }
    }

    bool TSDFVolumeOctree::reprojectPoint (const pcl::PointXYZ &pt, int &u, int &v) const {
        u = (pt.x * focal_length_x_ / pt.z) + principal_point_x_;
        v = (pt.y * focal_length_y_ / pt.z) + principal_point_y_;
        return (pt.z > 0 && u >= 0 && u < image_width_ && v >= 0 && v < image_height_);
    }

    void TSDFVolumeOctree::getFrustumCulledVoxels (const Eigen::Affine3d &trans,
                                            std::vector<OctreeNode::Ptr> &voxels) const {
        std::vector<OctreeNode::Ptr> voxels_all;
        octree_->getLeaves (voxels_all, max_cell_size_x_, max_cell_size_y_, max_cell_size_z_);
        pcl::PointCloud<pcl::PointXYZ>::Ptr voxel_cloud(new pcl::PointCloud<pcl::PointXYZ> (voxels_all.size(), 1));
        std::vector<int> indices;
        for (size_t i = 0; i < voxel_cloud->size(); ++i){
            pcl::PointXYZ &pt = voxel_cloud->at(i);
            voxels_all[i]->getCenter(pt.x, pt.y, pt.z);
        }
        pcl::FrustumCulling<pcl::PointXYZ> fc (false);
        Eigen::Matrix4f cam2robot;
        cam2robot << 0, 0, 1, 0,
                     0, -1, 0, 0,
                     1, 0, 0, 0,
                     0, 0, 0, 1;
        Eigen::Matrix4f trans_robot = trans.matrix().cast<float>()*cam2robot;
        fc.setCameraPose (trans_robot);
        //Set the FOV a little wider to account for overlap
        fc.setHorizontalFOV (70);
        fc.setVerticalFOV (70);
        fc.setNearPlaneDistance (min_sensor_dist_);
        fc.setFarPlaneDistance (max_sensor_dist_);
        fc.setInputCloud (voxel_cloud);
        fc.filter (indices);
        voxels.resize (indices.size());
        for (size_t i = 0 ; i < indices.size() ; i++) {
            voxels[i] = voxels_all[indices[i]];
        }
    }

    bool TSDFVolumeOctree::getFxn (const pcl::PointXYZ &pt, float &val) const {
        std::vector<const OctreeNode*> neighbors;
        std::vector<pcl::PointXYZ> centers;
        if (!getNeighbors (pt, neighbors, centers))
            return (false);
        val = 0;
        float c = xsize_ / xres_;
        for (size_t i = 0 ; i < neighbors.size() ; i++) {
            const OctreeNode* vox = neighbors[i];
            float ctrx, ctry, ctrz;
            vox->getCenter (ctrx, ctry, ctrz);
            val += (c - fabs (pt.x -ctrx) ) * (c - fabs (pt.y - ctry) ) * (c - fabs (pt.z-ctrz)) * vox->d_;
        }
        val /= (c*c*c);
        return true;
    }

    bool TSDFVolumeOctree::getGradient (const pcl::PointXYZ &pt, Eigen::Vector3f &grad) const  {
        std::vector<const OctreeNode*> neighbors;
        std::vector<pcl::PointXYZ> centers;
        if (!getNeighbors (pt, neighbors, centers))
            return (false);
        grad (0) = grad (1) = grad (2) = 0;
        float c = xsize_ / xres_;
        for (size_t i = 0; i < neighbors.size(); i++) {
            const OctreeNode* vox = neighbors[i];
            float ctrx, ctry, ctrz;
            vox->getCenter (ctrx, ctry, ctrz);
            grad(0) += -sgn (pt.x - ctrx) * (c - fabs (pt.y - ctry)) * (c - fabs (pt.z - ctrz)) * vox->d_;
            grad(1) += (c - fabs (pt.x - ctrx)) * -sgn (pt.y - ctry) * (c - fabs (pt.z - ctrz)) * vox->d_;
            grad(2) += (c - fabs (pt.x - ctrx)) * (c - fabs (pt.y - ctry)) * -sgn (pt.z - ctrz) * vox->d_;
        }
        grad /= (c*c*c);
        return (true);
    }

    bool TSDFVolumeOctree::getHessian (const pcl::PointXYZ &pt, Eigen::Matrix3f &hessian) const  {
        std::vector<const OctreeNode *> neighbors;
        std::vector<pcl::PointXYZ> centers;
        if (!getNeighbors (pt, neighbors, centers))
            return (false);
        hessian.setZero();
        float c = xsize_ / xres_;
        for (size_t i = 0; i < neighbors.size() ; i++) {
            const OctreeNode* vox = neighbors[i];
            const pcl::PointXYZ &ctr = centers[i];
            hessian (0,1) += sgn(pt.x - ctr.x)*sgn (pt.y - ctr.y)*(c - fabs (pt.z - ctr.z)) * vox->d_;
            hessian (0,2) += sgn(pt.x - ctr.x)*(c - fabs (pt.y - ctr.y))*sgn (pt.z - ctr.z) * vox->d_;
            hessian (1,2) += (c - fabs(pt.x - ctr.x))*sgn (pt.y - ctr.y)*sgn (pt.z - ctr.z) * vox->d_;
        }
        hessian /= (c*c*c);
        hessian (1,0) = hessian (0,1);
        hessian (2,0) = hessian (0,2);
        hessian (2,1) = hessian (1,2);
        return (true);
    }

    bool TSDFVolumeOctree::getFxnAndGradient (const pcl::PointXYZ &pt, float &val, Eigen::Vector3f &grad) const {
        std::vector<const OctreeNode*> neighbors;
        std::vector<pcl::PointXYZ> centers;
        if (!getNeighbors (pt, neighbors, centers))
            return (false);
        val = 0;
        grad (0) = grad (1) = grad (2) = 0;
        float c = xsize_ / xres_;
        if (neighbors.size() != 8)
            return (false);
        for (size_t i =0; i < neighbors.size(); i++) {
            const OctreeNode* vox = neighbors[i];
            const pcl::PointXYZ &ctr = centers[i];
            val += (c - fabs (pt.x - ctr.x)) * (c - fabs (pt.y - ctr.y)) * (c - fabs (pt.z - ctr.z)) * vox->d_;
            grad(0) += -sgn (pt.x - ctr.x) * (c - fabs (pt.y - ctr.y)) * (c - fabs (pt.z - ctr.z)) * vox->d_;
            grad(1) += (c - fabs (pt.x - ctr.x)) * -sgn (pt.y - ctr.y) * (c - fabs (pt.z - ctr.z)) * vox->d_;
            grad(2) += (c - fabs (pt.x - ctr.x)) * (c - fabs (pt.y - ctr.y)) * -sgn (pt.z - ctr.z) * vox->d_;
        }
        val /= (c*c*c);
        grad /= (c*c*c);
        return (true);
    }

    bool TSDFVolumeOctree::getFxnGradientAndHessian (
            const pcl::PointXYZ &pt,
            float &val,
            Eigen::Vector3f &grad,
            Eigen::Matrix3f &hessian) const {
        std::vector<const OctreeNode*> neighbors;
        std::vector<pcl::PointXYZ> centers;
        if (!getNeighbors (pt, neighbors, centers))
            return (false);
        val = 0;
        grad (0) = grad (1) = grad (2) = 0;
        hessian.setZero();
        float c = xsize_/xres_;
        if (neighbors.size() !=8)
            return (false);
        for (size_t i = 0; i < neighbors.size(); i++) {
            const OctreeNode* vox = neighbors[i];
            const pcl::PointXYZ &ctr = centers[i];
            val += (c - fabs (pt.x - ctr.x)) * (c - fabs (pt.y - ctr.y)) * (c - fabs (pt.z - ctr.z)) * vox->d_;
            grad(0) += -sgn (pt.x - ctr.x) * (c - fabs (pt.y - ctr.y)) * (c - fabs (pt.z - ctr.z)) * vox->d_;
            grad(1) += (c - fabs (pt.x - ctr.x)) * -sgn (pt.y - ctr.y) * (c - fabs (pt.z - ctr.z)) * vox->d_;
            grad(2) += (c - fabs (pt.x - ctr.x)) * (c - fabs (pt.y - ctr.y)) * -sgn (pt.z - ctr.z) * vox->d_;
            hessian (0,1) += sgn(pt.x - ctr.x)*sgn(pt.y - ctr.y)*(c-fabs (pt.z - ctr.z)) * vox->d_;
            hessian (0,2) += sgn(pt.x - ctr.x)*(c - fabs (pt.y - ctr.y))*sgn(pt.z - ctr.z) * vox->d_;
            hessian (1,2) += (c - fabs(pt.x - ctr.x))*sgn (pt.y - ctr.y)*sgn(pt.z - ctr.z) * vox->d_;
        }
        val /= (c*c*c);
        grad /= (c*c*c);
        hessian /= (c*c*c);
        hessian (1,0) = hessian (0,1);
        hessian (2,0) = hessian (0,2);
        hessian (2,1) = hessian (1,2);
        return (true);
    }

    bool TSDFVolumeOctree::getNeighbors (const pcl::PointXYZ &pt, std::vector<const OctreeNode*> &neighbors,
                                    std::vector<pcl::PointXYZ> &centers) const {
        int xi, yi, zi;
        if (!getVoxelIndex (pt.x, pt.y, pt.z, xi, yi, zi))
            return (false);
        pcl::PointXYZ v = getVoxelCenter (xi, yi, zi);
        if (pt.x < v.x) xi -= 1;
        if (pt.y < v.y) yi -= 1;
        if (pt.z < v.z) zi -= 1;
        if (xi < 0 || xi >= xres_-1 || yi < 0 || yi >= yres_ -1 || zi < 0 || zi >= zres_ -1)
            return (false);
        for (int dx = 0; dx <= 1; dx++) {
            for (int dy = 0; dy <= 1; dy++) {
                for (int dz = 0; dz <= 1; dz++) {
                    pcl::PointXYZ v = getVoxelCenter (xi+dx, yi+dy, zi+dz);
                    const OctreeNode* vox = octree_->getContainingVoxel (v.x, v.y, v.z);
                    if(!vox)
                        return (false);
                    neighbors.push_back(vox);
                    centers.push_back(v);
                }
            }
        }
        return (true);
    }
}

