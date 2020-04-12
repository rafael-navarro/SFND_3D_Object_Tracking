# Final Report 
## FP.1 Match 3D Objects
Implementation is done in "matchBoundingBoxes" function. In order to obtain the matching bounding boxes,
algorithm iterates over all matching keypoints, finding the containing bbox of each keypoint for both frames. Then a nested map is used to keep count of pairs of bbox matching. Finally, for each bbox is search for the bbox with the highest ocurrence (if tie, first is returned).
  