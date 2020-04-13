
#include <iostream>
#include <algorithm>
#include <numeric>
#include <opencv2/highgui/highgui.hpp>
#include <opencv2/imgproc/imgproc.hpp>

#include "camFusion.hpp"
#include "dataStructures.h"

using namespace std;


// Create groups of Lidar points whose projection into the camera falls into the same bounding box
void clusterLidarWithROI(std::vector<BoundingBox> &boundingBoxes, std::vector<LidarPoint> &lidarPoints, float shrinkFactor, cv::Mat &P_rect_xx, cv::Mat &R_rect_xx, cv::Mat &RT)
{
    // loop over all Lidar points and associate them to a 2D bounding box
    cv::Mat X(4, 1, cv::DataType<double>::type);
    cv::Mat Y(3, 1, cv::DataType<double>::type);

    for (auto it1 = lidarPoints.begin(); it1 != lidarPoints.end(); ++it1)
    {
        // assemble vector for matrix-vector-multiplication
        X.at<double>(0, 0) = it1->x;
        X.at<double>(1, 0) = it1->y;
        X.at<double>(2, 0) = it1->z;
        X.at<double>(3, 0) = 1;

        // project Lidar point into camera
        Y = P_rect_xx * R_rect_xx * RT * X;
        cv::Point pt;
        pt.x = Y.at<double>(0, 0) / Y.at<double>(0, 2); // pixel coordinates
        pt.y = Y.at<double>(1, 0) / Y.at<double>(0, 2);

        vector<vector<BoundingBox>::iterator> enclosingBoxes; // pointers to all bounding boxes which enclose the current Lidar point
        for (vector<BoundingBox>::iterator it2 = boundingBoxes.begin(); it2 != boundingBoxes.end(); ++it2)
        {
            // shrink current bounding box slightly to avoid having too many outlier points around the edges
            cv::Rect smallerBox;
            smallerBox.x = (*it2).roi.x + shrinkFactor * (*it2).roi.width / 2.0;
            smallerBox.y = (*it2).roi.y + shrinkFactor * (*it2).roi.height / 2.0;
            smallerBox.width = (*it2).roi.width * (1 - shrinkFactor);
            smallerBox.height = (*it2).roi.height * (1 - shrinkFactor);

            // check wether point is within current bounding box
            if (smallerBox.contains(pt))
            {
                enclosingBoxes.push_back(it2);
            }

        } // eof loop over all bounding boxes

        // check wether point has been enclosed by one or by multiple boxes
        if (enclosingBoxes.size() == 1)
        { 
            // add Lidar point to bounding box
            enclosingBoxes[0]->lidarPoints.push_back(*it1);
        }

    } // eof loop over all Lidar points
}


void show3DObjects(std::vector<BoundingBox> &boundingBoxes, cv::Size worldSize, cv::Size imageSize, float distance, string label, bool bWait)
{
    // create topview image
    cv::Mat topviewImg(imageSize, CV_8UC3, cv::Scalar(255, 255, 255));

    for(auto it1=boundingBoxes.begin(); it1!=boundingBoxes.end(); ++it1)
    {
        // create randomized color for current 3D object
        cv::RNG rng(it1->boxID);
        cv::Scalar currColor = cv::Scalar(rng.uniform(0,150), rng.uniform(0, 150), rng.uniform(0, 150));

        // plot Lidar points into top view image
        int top=1e8, left=1e8, bottom=0.0, right=0.0; 
        float xwmin=1e8, ywmin=1e8, ywmax=-1e8;
        for (auto it2 = it1->lidarPoints.begin(); it2 != it1->lidarPoints.end(); ++it2)
        {
            // world coordinates
            float xw = (*it2).x; // world position in m with x facing forward from sensor
            float yw = (*it2).y; // world position in m with y facing left from sensor
            xwmin = xwmin<xw ? xwmin : xw;
            ywmin = ywmin<yw ? ywmin : yw;
            ywmax = ywmax>yw ? ywmax : yw;

            // top-view coordinates
            int y = (-xw * imageSize.height / worldSize.height) + imageSize.height;
            int x = (-yw * imageSize.width / worldSize.width) + imageSize.width / 2;

            // find enclosing rectangle
            top = top<y ? top : y;
            left = left<x ? left : x;
            bottom = bottom>y ? bottom : y;
            right = right>x ? right : x;

            // draw individual point
            cv::circle(topviewImg, cv::Point(x, y), 4, currColor, -1);
        }

        // draw enclosing rectangle
        cv::rectangle(topviewImg, cv::Point(left, top), cv::Point(right, bottom),cv::Scalar(0,0,0), 2);

        // augment object with some key data
        char str1[200], str2[200];
        sprintf(str1, "id=%d, #pts=%d", it1->boxID, (int)it1->lidarPoints.size());
        putText(topviewImg, str1, cv::Point2f(left-250, bottom+50), cv::FONT_ITALIC, 2, currColor);
        sprintf(str2, "xmin=%2.2f m, yw=%2.2f m", xwmin, ywmax-ywmin);
        putText(topviewImg, str2, cv::Point2f(left-250, bottom+125), cv::FONT_ITALIC, 2, currColor);  
        putText(topviewImg, label, cv::Point2f(250,50), cv::FONT_ITALIC, 2, currColor);  
    }

    // plot distance markers
    float lineSpacing = 2.0; // gap between distance markers
    int nMarkers = floor(worldSize.height / lineSpacing);
    for (size_t i = 0; i < nMarkers; ++i)
    {
        int y = (-(i * lineSpacing) * imageSize.height / worldSize.height) + imageSize.height;
        cv::line(topviewImg, cv::Point(0, y), cv::Point(imageSize.width, y), cv::Scalar(255, 0, 0));
    }
    int y = (-distance * imageSize.height / worldSize.height) + imageSize.height;
    cv::line(topviewImg, cv::Point(0, y), cv::Point(imageSize.width, y), cv::Scalar(0, 0, 255));

    // display image
    string windowName = "3D Objects";
    cv::namedWindow(windowName, 1);
    cv::imshow(windowName, topviewImg);

    if(bWait)
    {
        cv::waitKey(0); // wait for key to be pressed
    }
}


// associate a given bounding box with the keypoints it contains
void clusterKptMatchesWithROI(BoundingBox &boundingBox, std::vector<cv::KeyPoint> &kptsPrev, 
std::vector<cv::KeyPoint> &kptsCurr, std::vector<cv::DMatch> &kptMatches)
{
    int maxShift = 10;
    int shiftTolerance = 5;

    boundingBox.keypoints.clear();
    boundingBox.kptMatches.clear();

    //cout << "clusterKptMatchesWithROI" <<endl;

    float avgShift = 0;
    unsigned int counter = 0;

    //Compute average shift btw matches
    for(auto it = kptMatches.begin(); it != kptMatches.end(); ++it)
    {
        cv::KeyPoint kpCurr = kptsCurr.at(it->trainIdx);
        cv::KeyPoint kpPrev = kptsPrev.at(it->queryIdx);

        double shift = cv::norm(kpPrev.pt - kpCurr.pt);

        if(boundingBox.roi.contains(kpCurr.pt) && shift <= maxShift)
        {
            avgShift += shift;
            counter++;
        }
    }
    if(counter == 0)
    {
        cout << "Error: No elements found" << endl;
    }
    avgShift = avgShift / counter;
    //cout << "AvgShift:" << avgShift << "Tol: " << shiftTolerance << endl;  

    for(auto it = kptMatches.begin(); it != kptMatches.end(); ++it)
    {
        cv::KeyPoint kpCurr = kptsCurr.at(it->trainIdx);
        cv::KeyPoint kpPrev = kptsPrev.at(it->queryIdx);

        double shift = cv::norm(kpPrev.pt - kpCurr.pt);

        if(boundingBox.roi.contains(kpCurr.pt) &&
            (shift >= avgShift - shiftTolerance) && (shift <= avgShift + shiftTolerance))
        {
            //cout << "Added point " << " " << shift << endl;
            boundingBox.keypoints.push_back(kpCurr);
            boundingBox.kptMatches.push_back(*it);
        }     
    }
}


// Compute time-to-collision (TTC) based on keypoint correspondences in successive images
void computeTTCCamera(std::vector<cv::KeyPoint> &kptsPrev, std::vector<cv::KeyPoint> &kptsCurr, 
                      std::vector<cv::DMatch> kptMatches, double frameRate, double &TTC, cv::Mat *visImg)
{
    double minDist = 100.0; // min. required distance
    vector<double> distRatios;

    for (auto it1 = kptMatches.begin(); it1 != kptMatches.end() - 1; ++it1)
    { 
        // get current keypoint and its matched partner in the prev. frame
        cv::KeyPoint kpOuterCurr = kptsCurr.at(it1->trainIdx);
        cv::KeyPoint kpOuterPrev = kptsPrev.at(it1->queryIdx);

        for (auto it2 = it1 + 1; it2 != kptMatches.end(); ++it2)
        //for (auto it2 = kptMatches.begin() + 1; it2 != kptMatches.end(); ++it2)
        { 
            // get next keypoint and its matched partner in the prev. frame
            cv::KeyPoint kpInnerCurr = kptsCurr.at(it2->trainIdx);
            cv::KeyPoint kpInnerPrev = kptsPrev.at(it2->queryIdx);

            // compute distances and distance ratios
            double distCurr = cv::norm(kpOuterCurr.pt - kpInnerCurr.pt);
            double distPrev = cv::norm(kpOuterPrev.pt - kpInnerPrev.pt);

            if (distPrev > std::numeric_limits<double>::epsilon() && distCurr >= minDist)
            { // avoid division by zero

                double distRatio = distCurr / distPrev;
                distRatios.push_back(distRatio);
            }
        } // eof inner loop over all matched kpts
    } 

    if (distRatios.size() == 0)
    {
        TTC = NAN;
        return;
    }

    // compute camera-based TTC from distance ratios
    double modeDistRatio;
	std::sort(distRatios.begin(), distRatios.end());
  	int medianIdx = int(distRatios.size() / 2);
    if(distRatios.size() % 2 != 0)
      modeDistRatio = distRatios[medianIdx];
    else
      modeDistRatio = (distRatios[medianIdx - 1] + distRatios[medianIdx]) / 2;
    
    
    double dT = 1 / frameRate;
    TTC = -dT / (1 - modeDistRatio);
}

float findDistanceRobust(std::vector<LidarPoint> &points)
{
    //cout << "findDistanceRobust " << points.size() << endl;
    int MaxNumElements = 50;
    std::vector<double> distances;

    cerr << "Lidar Points: "; 
    for(LidarPoint point : points)
    {
        //double distance = sqrt(point.x * point.x + point.y * point.y + point.z * point.z);
        //if(point.r > 0.0)
        distances.push_back(point.x);
        cerr << point.x << " ";
    }
    cerr << endl;

    if(distances.size() == 0)
    {
        cout << "Error empty points" << endl;
        return NAN;
    }
    
    //std::sort(distances.begin(), distances.end());
    //unsigned int numElems = min(MaxNumElements, (int) distances.size());
    //float avg = std::accumulate(distances.begin(), distances.begin() + numElems - 1, 0.0) / numElems;
    //return avg;

    double median;
	std::sort(distances.begin(), distances.end());
  	int medianIdx = int(distances.size() / 2);
    if(distances.size() % 2 != 0)
      median = distances[medianIdx];
    else
      median = (distances[medianIdx - 1] + distances[medianIdx]) / 2;

    return median;

}

void computeTTCLidar(std::vector<LidarPoint> &lidarPointsPrev,
                     std::vector<LidarPoint> &lidarPointsCurr, double frameRate, double &TTC, double &distPrev, double &distCurr)
{
    //cout << "computeTTCLidar" << endl;
    double dT = 1 / frameRate; // time between two measurements in seconds
    
    distPrev = findDistanceRobust(lidarPointsPrev);
    distCurr = findDistanceRobust(lidarPointsCurr);

    TTC = distCurr * dT / (distPrev - distCurr);
    cout << "computeTTCLidar.TTC:" << TTC << endl;
}

bool getContainingBBoxInKpt(std::vector<BoundingBox> &matchBoundingBoxes, std::vector<cv::KeyPoint> &keypoints, int kptIdx, int &bboxIdx)
{
    for(auto it = matchBoundingBoxes.begin(); it != matchBoundingBoxes.end(); ++it)
    {
        cv::KeyPoint kpCurr = keypoints.at(kptIdx);
        if(it->roi.contains(kpCurr.pt))
        {
            bboxIdx = it->boxID;
                
            //cout << "Found " << bboxIdx << endl;
            return true;
        }
    }

    //cout << "Not Found " << endl;
    return false;
}

void matchBoundingBoxes(std::vector<cv::DMatch> &matches, std::map<int, int> &bbBestMatches, DataFrame &prevFrame, DataFrame &currFrame)
{
    std::map<int, std::map<int, unsigned int>> bbMatches;

    //cout << "matchBoundingBoxes " << endl; 
    //Find bboxs containing points and keep count of prev and curr rois
    for(auto it = matches.begin(); it != matches.end(); ++it)
    {
        int prevBBox, currBBox;
        if(getContainingBBoxInKpt(prevFrame.boundingBoxes, prevFrame.keypoints, it->queryIdx, prevBBox) && 
            getContainingBBoxInKpt(currFrame.boundingBoxes, currFrame.keypoints, it->trainIdx, currBBox))
        {
            //Counters is keep in a nested map
            ++bbMatches[prevBBox][currBBox];
        }
    }
    
    //cout << "bbMAtches " << bbMatches.size() << endl; 

    //Search for the most ocurring bbox matchings.
    for(auto it = bbMatches.begin(); it != bbMatches.end(); ++it)
    {
        int currMax = 0, idxMax = -1;
        for(auto it2 = it->second.begin(); it2 != it->second.end(); ++it2)
        {
            if(it2->second > currMax)
            {
                idxMax = it2->first;
                currMax = it2->second;
            }
        }
        bbBestMatches[it->first] = idxMax;
        //cout << "BBoxs matching found " << it->first << " " << idxMax << endl;
    }
}
