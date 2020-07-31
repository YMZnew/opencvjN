#include <iostream>

#include <emscripten/emscripten.h>

#include <opencv2/opencv.hpp>
#include <opencv2/core.hpp>
#include <opencv2/core/types_c.h>
#include <opencv2/imgproc/imgproc.hpp>
#include <opencv2/calib3d/calib3d.hpp>
#include <opencv2/features2d.hpp>

using namespace std;
using namespace cv;

#define GOOD_MATCH_RATIO    0.7f
#define MAX_FEATURES        2000

bool initialized = false;

Ptr<ORB> orb = NULL;
Ptr<BFMatcher> matcher = NULL;

Mat refGray, refDescr;

vector<KeyPoint> refKeyPts;
vector<Point2f> corners(4);

static Mat im_gray(uchar data[], size_t cols, size_t rows) {
    uint32_t idx;
    uchar gray[rows][cols];
    for (int i = 0; i < rows; ++i) {
        for (int j = 0; j < cols; ++j) {
            idx = (i * cols * 4) + j * 4;

            // rgba to rgb
            uchar r = data[idx];
            uchar g = data[idx + 1];
            uchar b = data[idx + 2];
            // uchar a = data[idx + 3];

            // turn frame image to gray scale
            gray[i][j] = (0.30 * r) + (0.59 * g) + (0.11 * b);
        }
    }

    return Mat(rows, cols, CV_8UC1, gray);
}

static void fill_output(double *output, vector<Point2f> warped, Mat H) {
    output[0] = H.at<double>(0,0);
    output[1] = H.at<double>(0,1);
    output[2] = H.at<double>(0,2);
    output[3] = H.at<double>(1,0);
    output[4] = H.at<double>(1,1);
    output[5] = H.at<double>(1,2);
    output[6] = H.at<double>(2,0);
    output[7] = H.at<double>(2,1);
    output[8] = H.at<double>(2,2);

    output[9]  = warped[0].x;
    output[10] = warped[0].y;
    output[11] = warped[1].x;
    output[12] = warped[1].y;
    output[13] = warped[2].x;
    output[14] = warped[2].y;
    output[15] = warped[3].x;
    output[16] = warped[3].y;
}

EMSCRIPTEN_KEEPALIVE
void initAR(uchar refData[], size_t refCols, size_t refRows) {
    orb = ORB::create(MAX_FEATURES);
    matcher = BFMatcher::create();

    Mat refGray = im_gray(refData, refCols, refRows);
    orb->detectAndCompute(refGray, noArray(), refKeyPts, refDescr);

    corners[0] = cvPoint( 0, 0 );
    corners[1] = cvPoint( refCols, 0 );
    corners[2] = cvPoint( refCols, refRows );
    corners[3] = cvPoint( 0, refRows );

    initialized = true;
    cout << "Ready!" << endl;
}

EMSCRIPTEN_KEEPALIVE
double *performAR(uchar frameData[], size_t frameCols, size_t frameRows) {
    static Mat H, frameOld;
    static bool tracking = false;
    static short numMatches = 0, frames = 0;
    static vector<Point2f> framePts;
    static vector<Point2f> warped(4);

    // 9 from homography matrix, 8 from warped corners
    double *result = new double[17];

    if (!initialized) {
        cout << "Reference image not found. AR is unintialized!" << endl;
        return result;
    }

    Mat transform;
    Mat frameGray = im_gray(frameData, frameCols, frameRows);
    // GaussianBlur(frameGray, frameGray, Size(5,5), 2);

    if (!tracking) {
        framePts.clear();

        Mat frameDescr;
        vector<KeyPoint> frameKeyPts;
        orb->detectAndCompute(frameGray, noArray(), frameKeyPts, frameDescr);

        vector<vector<DMatch>> knnMatches;
        matcher->knnMatch(frameDescr, refDescr, knnMatches, 2);

        vector<Point2f> refPts;
        // find the best matches
        for (size_t i = 0; i < knnMatches.size(); ++i) {
            if (knnMatches[i][0].distance < GOOD_MATCH_RATIO * knnMatches[i][1].distance) {
                framePts.push_back( frameKeyPts[knnMatches[i][0].queryIdx].pt );
                refPts.push_back( refKeyPts[knnMatches[i][0].trainIdx].pt );
            }
        }

        numMatches = framePts.size();
        // need at least 4 pts to define homography
        if (numMatches > 15) {
            H = findHomography(refPts, framePts, RANSAC);
            tracking = true;
        }
    }
    else {
        vector<float> err;
        vector<uchar> status;
        vector<Point2f> newPts, goodPtsNew, goodPtsOld;
        calcOpticalFlowPyrLK(frameOld, frameGray, framePts, newPts, status, err);
        for (size_t i = 0; i < framePts.size(); ++i) {
            if (status[i]) {
                goodPtsNew.push_back(newPts[i]);
                goodPtsOld.push_back(framePts[i]);
            }
        }

        if (!goodPtsNew.empty() && goodPtsNew.size() > 2*numMatches/3) {
            transform = estimateAffine2D(goodPtsOld, goodPtsNew);

            // add row of [0,0,1] to transform to make it 3x3
            Mat row = Mat::zeros(1, 3, CV_64F);
            row.at<double>(0,2) = 1.0;
            transform.push_back(row);

            // update homography matrix
            H = transform * H;

            // set old points to new points
            framePts = goodPtsNew;
        }
        else {
            tracking = false; // stop tracking if new points are insufficient
        }
    }

    if (tracking) {
        perspectiveTransform(corners, warped, H);
        fill_output(result, warped, H);
    }

    frameOld = frameGray.clone();

    return result;
}
