
#include <Mapper.h>

#include <fstream>
#include <vector>
#include <iostream>
#include <queue>
#include <functional>
#include <Eigen/Dense>

#include <CommonDebugFunction.h>
#include <SingletonGameState.h>

#define AABB_LOG_DEBUG false

Mapper::Mapper(MapSpatialInfos map /* = MapSpatialInfos() */,
    float startAngleManaged /*= MAPPER_MIN_FIELD_VIEW*/,
    float endFieldOfView /* = MAPPER_MAX_FIELD_VIEW*/)
{
    std::cout << TAG << std::endl;
    setManagedAngleFieldOFView(startAngleManaged, endFieldOfView);
    _end.store(false);
    _occupancy_grid = std::vector<std::vector<std::pair<bool, int>>>(
        map.grid_lenght,
        std::vector<std::pair<bool, int>>(map.grid_width, { false, 0 }));

    _mapCenterShifter = { map.get_grid_center_x(), map.get_grid_center_y(), 0 };
    //_mapCenterShifter = {0, 0, 0};

    _enablePathFinding.store(false);
}

Mapper::~Mapper()
{
}

void Mapper::setManagedAngleFieldOFView(float startAngleManaged, float endAngleManaged)
{
    std::cout << TAG << std::endl;
    _startAngleManaged = startAngleManaged;
    _endAngleManaged = endAngleManaged;
}

void Mapper::setMapInfos(MapSpatialInfos map /*  = MapSpatialInfos() */)
{
    std::cout << TAG << std::endl;
    _map = map;
}

void Mapper::startDataParsing()
{
    std::cout << TAG << std::endl;
    _end.store(false);
    _parserData = std::thread(&Mapper::loop_parseFieldPoints, this);
    _parserData.detach();
}

void Mapper::stopDataParsing()
{
    _end.store(true);
}

/// @brief Once enable, during parsing process, alone point will be filtered and skipped as artefact of the captor
///         point are considered "trusworthy" when there is at least _noiseFilter_nbrCorelationPoint point around them
/// @param enable
void Mapper::enableNoiseFilter(bool enable /*  = true */)
{
    _noiseFilter_Enable.store(enable);
}

/// @brief this allow you to specify the number of point needed around another point
///         fo it to be considered, and not discarded as Noise/artificat of the captor
void Mapper::setNoiseFilterNbrCorelationPoint(int nbr)
{
    _noiseFilter_nbrCorelationPoint.store(nbr);
}

// for debug

void Mapper::loop_parseFieldPoints(Mapper* myself)
{

    std::cout << TAG << std::endl;
    while (myself->_end.load() != true)
    {
        myself->_mutextDataToParse.lock();
        auto nbr = myself->_dataToParse.size();
        myself->_mutextDataToParse.unlock();
        if (nbr > 0)
        {
            myself->_mutextDataToParse.lock();
            std::cout << TAG << "_dataToParse contain:" << myself->_dataToParse.size() << std::endl;
            for (const auto& fieldpoints : myself->_dataToParse)
            {
                myself->processLidarData(fieldpoints.points);
            }
            myself->_dataToParse.clear();
            std::cout << TAG << "after" << myself->_dataToParse.size() << std::endl;
            myself->_mutextDataToParse.unlock();
        }
        else
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(MAPPER_DEFAULT_WAITING_TIME_PARSER_EMPTY_LIST));
        }
    }
}

/// @brief enabling the pathfinding will automatically calculate it to the destination after each data update
/// providing 
void Mapper::enablePathFinding(bool val)
{
    _enablePathFinding.store(val);
}

bool Mapper::isPathFindingEnabled()
{
    return _enablePathFinding.load();
}

std::vector<Eigen::Vector3f> Mapper::getCurrentPathfindingToDest()
{
    std::lock_guard<std::mutex> lock(_mutexPointsPathToDest);
    return _pointsPathToDest;
}

void Mapper::addDataToParse(const FieldPoints& fieldPoints)
{
    std::cout << TAG << std::endl;
    _mutextDataToParse.lock();
    _dataToParse.push_back(fieldPoints);
    _mutextDataToParse.unlock();
}

/// @brief Points are relative to the robot and are relative to it's rotation
/// @param point
void Mapper::parsePointToMap_pointsAreRelativeToRobot(const Point& point)
{
    // std::cout << TAG << std::endl;
    const auto absPoint = transformPointToGlobal(point);
    updateOccupancyGrid(absPoint);
}

/// @brief Points x,y are aboslute and does not depend of the robot position or rotation
/// @param point
void Mapper::parsePointToMap_pointsAreAbsolute(const Point& point)
{
}

// @brief used to convert a relative-to-robot point to an absolute one
// @param point
Point Mapper::transformPointToGlobal(const Point& point)
{
    Point ret = point;
    // Convert robot-relative point to a global map point
/*     float theta = _robotInfos._currentRobotAngle;
    float cos_theta = std::cos(theta);
    float sin_theta = std::sin(theta);

    ret.pos.x() = point.pos.x() * cos_theta - point.pos.y() * sin_theta + _robotInfos.center.x();
    ret.pos.y() = point.pos.x() * sin_theta + point.pos.y() * cos_theta + _robotInfos.center.y();
 */
    return ret;
}

void Mapper::updateOccupancyGrid(const Point& global_point)
{
    // Convert global point coordinates to grid cell indices
    int grid_x = static_cast<int>(std::round(global_point.pos.y() / _map.gridResolution)) + (_map.grid_width / 2);
    int grid_y = static_cast<int>(std::round(global_point.pos.y() / _map.gridResolution)) + (_map.grid_lenght / 2);
    if (grid_x >= 0 && grid_x < _map.grid_width && grid_y >= 0 && grid_y < _map.grid_lenght)
    {
        _occupancy_grid[grid_y][grid_x].second += 1;
    }
}

pcl::PointCloud<pcl::PointXYZ> Mapper::convertToPCLCloud(const std::vector<Point>& lidarPoints)
{
    auto cloud = pcl::PointCloud<pcl::PointXYZ>();
    for (const auto& pt : lidarPoints)
    {
        cloud.points.emplace_back(pt.pos.x(), pt.pos.y(), pt.pos.z());
    }
    cloud.width = cloud.points.size();
    cloud.height = 1;
    cloud.is_dense = true;
    return cloud;
}

/// @brief this function will basically noise filter the data and fill occupancyGrid
/// @param lidarPoints
void Mapper::processLidarData(const std::vector<Point>& lidarPoints)
{
    _mutexIsParsingData.lock();
    // Convert lidar data to PCL point cloud
    _parsingDataPointCloud = pcl::PointCloud<pcl::PointXYZ>::Ptr(new pcl::PointCloud<pcl::PointXYZ>());
    *_parsingDataPointCloud = convertToPCLCloud(lidarPoints);

    // 1. Noise Filtering using Statistical Outlier Removal
    _parsingDataPointCloudFiltered = pcl::PointCloud<pcl::PointXYZ>::Ptr(new pcl::PointCloud<pcl::PointXYZ>());
    pcl::StatisticalOutlierRemoval<pcl::PointXYZ> sor;
    sor.setInputCloud(_parsingDataPointCloud);
    sor.setMeanK(_noiseFilter_nbrCorelationPoint.load()); // Set number of neighbors to analyze
    sor.setStddevMulThresh(1.0);                          // Threshold for outliers
    sor.filter(*_parsingDataPointCloudFiltered);

    // 2. Clustering (filter) using Euclidean Cluster Extraction
    pcl::search::KdTree<pcl::PointXYZ>::Ptr tree(new pcl::search::KdTree<pcl::PointXYZ>());
    tree->setInputCloud(_parsingDataPointCloudFiltered);

    std::cout << "Before filter : cloud_filtered.size : " << _parsingDataPointCloudFiltered->size() << std::endl;
    std::vector<pcl::PointIndices> cluster_indices;
    pcl::EuclideanClusterExtraction<pcl::PointXYZ> ec;
    ec.setClusterTolerance(MAPPER_GRID_RESOLUTION_CLUSTER_DISTANCE_TOLERANCE);   // Distance tolerance for clustering
    ec.setMinClusterSize(MAPPER_GRID_RESOLUTION_CLUSTER_POINT);     // Minimum number of points for a valid cluster
    ec.setMaxClusterSize(MAPPER_GRID_RESOLUTION_CLUSTER_NUMBER_TOLERANCE); // Maximum number of points per cluster
    ec.setSearchMethod(tree);
    ec.setInputCloud(_parsingDataPointCloudFiltered);
    ec.extract(cluster_indices);

    // 3. Create an Occupancy Grid, really useful when we have big grid cell that can contain multiple point
    // in most lidar case we have one dot per cell
    std::cout << "cluster_indices.size : " << cluster_indices.size() << std::endl;
    if (cluster_indices.size() > 0)
    {
        std::vector<std::vector<int>> occupancyGrid(_map.grid_width, std::vector<int>(_map.grid_lenght, 0));
        for (const auto& cluster : cluster_indices)
        {
            for (const int& index : cluster.indices)
            {
                pcl::PointXYZ point = _parsingDataPointCloudFiltered->points[index];

                // Convert point coordinates to grid indices
                int gridX = static_cast<int>((point.x / _map.gridResolution) + _mapCenterShifter.x());
                int gridY = static_cast<int>((point.y / _map.gridResolution) + _mapCenterShifter.y());

                // Check bounds before writing to the grid
                if (gridX >= 0 && gridX < _map.grid_width && gridY >= 0 && gridY < _map.grid_lenght)
                {
                    occupancyGrid[gridX][gridY] += 1;
                }
            }
        }

        // refine ocupancy to ojects, so that we can easily calculate around it
        _mutexDetectedObject.lock();
        _refinedLastDetectedObject = _refinedCurrentDetectedObject;
        _refinedCurrentDetectedObject = refineMapToObjects(occupancyGrid);
        std::cout << TAG << "Refined objects:" << _refinedCurrentDetectedObject.size() << std::endl;
        _mutexDetectedObject.unlock();

        if (_enablePathFinding.load() == true) {
            auto rob_and_dest = getCenteredRobotAndGoal();
            std::lock_guard<std::mutex> lock(_mutexPointsPathToDest);
            recursiveCalculateNextPathPositionToGoal(rob_and_dest.first, rob_and_dest.second, _refinedCurrentDetectedObject, _pointsPathToDest);
        }

        // CommonDebugFunction::savePointCloudToFile(_robotInfos, desination_goal, *_parsingDataPointCloudFiltered, _pointsPathToDest, _refinedCurrentDetectedObject, "objectAndPath", -_mapCenterShifter);

    }
    else
    {
        std::cout << "Filtered point ended emptying the objects." << std::endl;
    }
    _mutexIsParsingData.unlock();
}


// we set the robot position to the center of our perception map because lidar data have as center the robot
// If we want to manage fixed object / boundary  that the lidar does not see, the good approach is to add these object with the shifter
// instead of shifting every single detected point at each detection
// the problem is we have to also shift the destination to our perception
std::pair<RobotSpatialInfos, Eigen::Vector3f> Mapper::getCenteredRobotAndGoal()
{
    auto robot = SingletonGameState::getInstance().getRobotInfos();
    auto dest = SingletonGameState::getInstance().getDestinationGoal();
    auto shiftDestAbsolutToCenterOfMap = (dest - robot.center);
    robot.center = _mapCenterShifter; // set to center of our c++ map
    dest = robot.center + shiftDestAbsolutToCenterOfMap; // we set the dest goal from a percption from the center
    return std::make_pair(robot, dest);
}

/* 
bool Mapper::isCellBlockedWithRobotSize(int x, int y,
    const std::vector<std::vector<int>>& grid)
{
    auto robotCellsSize = _robotInfos.getRobotSizeInGridCells(_map.gridResolution);
    int rows = grid.size();
    int cols = grid[0].size();
    // Check if any cell within the robot's footprint is occupied
    for (int dx = -robotCellsSize.first / 2; dx <= robotCellsSize.first / 2; ++dx)
    {
        for (int dy = -robotCellsSize.second / 2; dy <= robotCellsSize.second / 2; ++dy)
        {
            int nx = x + dx, ny = y + dy;
            if (nx < 0 || nx >= rows || ny < 0 || ny >= cols || grid[nx][ny] > 0)
            {
                return true; // Cell is blocked or out of bounds
            }
        }
    }
    return false;
}

std::vector<std::pair<int, int>> Mapper::findPath(
    const std::vector<std::vector<int>>& grid,
    int startX, int startY, int destX, int destY)
{
    int rows = grid.size();
    int cols = grid[0].size();

    auto heuristic = [](int x1, int y1, int x2, int y2)
        {
            return std::hypot(x2 - x1, y2 - y1);
        };
    std::cout << TAG << "1" << std::endl;

    // Priority queue for open list
    std::priority_queue<Node*, std::vector<Node*>, CompareNode> openList;
    std::unordered_map<int, Node*> allNodes;
    auto hash = [cols](int x, int y)
        { return x * cols + y; };

    // Start and destination nodes
    Node* startNode = new Node(startX, startY, 0, heuristic(startX, startY, destX, destY));
    openList.push(startNode);
    allNodes[hash(startX, startY)] = startNode;

    std::cout << TAG << "2" << std::endl;
    std::vector<std::pair<int, int>> directions{ {0, 1}, {1, 0}, {0, -1}, {-1, 0} };

    while (!openList.empty())
    {
        Node* current = openList.top();
        openList.pop();

        // Check if we reached the destination
        if (current->x == destX && current->y == destY)
        {
            std::vector<std::pair<int, int>> path;
            for (Node* node = current; node; node = node->parent)
            {
                path.emplace_back(node->x, node->y);
            }
            std::reverse(path.begin(), path.end());

            // Cleanup all nodes
            for (auto& [_, node] : allNodes)
                delete node;
            return path;
        }
        std::cout << TAG << "3" << std::endl;

        // Explore neighbors
        for (const auto& [dx, dy] : directions)
        {
            int nx = current->x + dx, ny = current->y + dy;

            // Check if the candidate cell is navigable
            if (nx >= 0 && nx < rows &&
                ny >= 0 && ny < cols &&
                !isCellBlockedWithRobotSize(nx, ny, grid))
            {
                float newCost = current->cost + 1.0f;
                int nodeHash = hash(nx, ny);

                if (allNodes.find(nodeHash) == allNodes.end() || newCost < allNodes[nodeHash]->cost)
                {
                    Node* neighbor = new Node(nx, ny, newCost, heuristic(nx, ny, destX, destY), current);
                    openList.push(neighbor);
                    allNodes[nodeHash] = neighbor;
                }
            }
        }
    }

    std::cout << TAG << "4" << std::endl;

    // Cleanup all nodes if no path is found
    for (auto& [_, node] : allNodes)
        delete node;
    return {}; // Return empty path if no route to destination
} */

std::vector<Object3D> Mapper::refineMapToObjects(
    const std::vector<std::vector<int>>& grid)
{
    int rows = grid.size();
    int cols = grid[0].size();

    std::cout << TAG << "rows:" << rows << ", cols:" << cols << std::endl;

    std::vector<std::vector<bool>> visited(rows, std::vector<bool>(cols, false));
    std::vector<Object3D> objects;

    auto isValid = [&](int x, int y)
        {
            return x >= 0 && x < rows && y >= 0 && y < cols && grid[x][y] > 0 && !visited[x][y];
        };

    std::vector<std::pair<int, int>> directions = { {0, 1}, {1, 0}, {0, -1}, {-1, 0} };

    for (int x = 0; x < rows; ++x)
    {
        for (int y = 0; y < cols; ++y)
        {
            if (grid[x][y] > 0 && !visited[x][y])
            {
                // Start a BFS to find all connected cells of the current cluster
                std::queue<std::pair<int, int>> q;
                q.push({ x, y });
                visited[x][y] = true;

                std::vector<std::pair<int, int>> clusterCells;

                while (!q.empty())
                {
                    auto [cx, cy] = q.front();
                    q.pop();
                    clusterCells.push_back({ cx, cy });

                    for (const auto& [dx, dy] : directions)
                    {
                        int nx = cx + dx, ny = cy + dy;
                        if (isValid(nx, ny))
                        {
                            visited[nx][ny] = true;
                            q.push({ nx, ny });
                        }
                    }
                }

                // Calculate object properties from the cluster
                int minX = rows, maxX = 0, minY = cols, maxY = 0;
                for (const auto& [cx, cy] : clusterCells)
                {
                    minX = std::min(minX, cx);
                    maxX = std::max(maxX, cx);
                    minY = std::min(minY, cy);
                    maxY = std::max(maxY, cy);
                }

                float centerX = ((minX + maxX) / 2.0f) * _map.gridResolution;
                float centerY = ((minY + maxY) / 2.0f) * _map.gridResolution;
                float length = (maxX - minX + 1) * _map.gridResolution;
                float width = (maxY - minY + 1) * _map.gridResolution;

                objects.push_back(Object3D{
                    Eigen::Vector3f(centerX, centerY, 0.0f),            // Assuming 2D map; z = 0
                    Eigen::Vector3f(length, width, _map.gridResolution) // Assuming uniform grid resolution for height
                    });
            }
        }
    }

    return objects;
}

// deprecated, do not use
bool Mapper::isInCollision(const Eigen::Vector3f& position, const Eigen::Vector3f& size, const Eigen::Vector3f& point)
{
    Eigen::Vector3f halfSize = size * 0.5f;
    return (point.x() >= (position.x() - halfSize.x()) &&
        point.x() <= (position.x() + halfSize.x()) &&
        point.y() >= (position.y() - halfSize.y()) &&
        point.y() <= (position.y() + halfSize.y()));
}

// check if there is a collision from the moving object (robot) as start point
// with the object in bewtween (the immobile object)
// uses the slab method to determine if the line segment intersects the AABB (Axis-Aligned Bounding Box).
// AABB only work with rectangle that are aligned with the x and Y axis !!!
// If you want to check collision for non-aligned ovject, use SAT
bool Mapper::lineIntersectsAABB(const Object3D& movingObj,
    const Object3D& immobileObj,
    const Eigen::Vector3f& destination)
{
    // we increase the immobile object by the mooving on (half size) to take in account it's size
    // like the robot size, and only use a vector as calculation
    Eigen::Vector3f imObjHalfSize = (immobileObj.size + movingObj.size) * 0.5f;

    Eigen::Vector3f direction = destination - movingObj.center;
    Eigen::Vector3f invertDirection = direction.cwiseInverse();
    if (AABB_LOG_DEBUG)
        std::cout << "DEBUG direction:[" << direction[0] << "][" << direction[1] << "][" << direction[2] << "] , " << "invertDirection:[" << invertDirection[0] << "][" << invertDirection[1] << "][" << invertDirection[2] << "] , " << std::endl;

    if (AABB_LOG_DEBUG)
        std::cout << "DEBUG immobileObj.center:[" << immobileObj.center[0] << "][" << immobileObj.center[1] << "][" << immobileObj.center[2] << "] , " << "imObjHalfSize:[" << imObjHalfSize[0] << "][" << imObjHalfSize[1] << "][" << imObjHalfSize[2] << "] , " << "movingObj.center:[" << movingObj.center[0] << "][" << movingObj.center[1] << "][" << movingObj.center[2] << "] , " << std::endl;

    // We take the distance between these 2 object and add/minus the cumulation of their size
    Eigen::Vector3f tmptMin = (immobileObj.center - imObjHalfSize - movingObj.center);
    Eigen::Vector3f tmptMax = (immobileObj.center + imObjHalfSize - movingObj.center);
    if (AABB_LOG_DEBUG)
        std::cout << "DEBUG tmptMin:[" << tmptMin[0] << "][" << tmptMin[1] << "][" << tmptMin[2] << "]" << std::endl;
    if (AABB_LOG_DEBUG)
        std::cout << "DEBUG tmptMax:[" << tmptMax[0] << "][" << tmptMax[1] << "][" << tmptMax[2] << "]" << std::endl;

    Eigen::Vector3f tMin = tmptMin.cwiseProduct(invertDirection);
    Eigen::Vector3f tMax = tmptMax.cwiseProduct(invertDirection);
    if (AABB_LOG_DEBUG)
        std::cout << "DEBUG tMin:[" << tMin[0] << "][" << tMin[1] << "][" << tMin[2] << "]" << std::endl;
    if (AABB_LOG_DEBUG)
        std::cout << "DEBUG tMax:[" << tMax[0] << "][" << tMax[1] << "][" << tMax[2] << "]" << std::endl;

    Eigen::Vector3f t1 = tMin.cwiseMin(tMax);
    Eigen::Vector3f t2 = tMin.cwiseMax(tMax);
    if (AABB_LOG_DEBUG)
        std::cout << "DEBUG t1:[" << t1[0] << "][" << t1[1] << "][" << t1[2] << "]" << std::endl;
    if (AABB_LOG_DEBUG)
        std::cout << "DEBUG t2:[" << t2[0] << "][" << t2[1] << "][" << t2[2] << "]" << std::endl;

    float tNear = std::max(std::max(t1.x(), t1.y()), t1.z());
    float tFar = std::min(std::min(t2.x(), t2.y()), t2.z());
    if (AABB_LOG_DEBUG)
        std::cout << "DEBUG tNear:[" << tNear << "], tFar:[" << tFar << "]" << std::endl
        << std::endl;

    return (tNear <= tFar && tFar >= 0);
}

/// @brief this function from a starting pos, the colliding object and the destination, will return the closer to reach.
/// to be precis, we get the 2 border nearest to the start pos, we then check wich border is nearest to the destination, then return it
/// it allow you to go around an object easily (if you want to do smooth deplacmement and skeep step, you need another smoothing function)
/// @param robot
/// @param collidingObject
/// @param destination
/// @return
Eigen::Vector3f Mapper::getCollidingNextPositionCloserBorderLogic(const RobotSpatialInfos& robot,
    const Object3D& collidingObject,
    const Eigen::Vector3f& destination)
{
    // Calculate corners of the colliding object (with robot size margin)
    Eigen::Vector3f halfSizeObj = collidingObject.size * 0.5f;
    Eigen::Vector3f halfSizeRobot = robot.size * 0.5f;
    Eigen::Vector3f expandedHalfSize = halfSizeObj + halfSizeRobot;

    std::vector<Eigen::Vector3f> corners = {
        collidingObject.center + Eigen::Vector3f(expandedHalfSize.x(), expandedHalfSize.y(), 0),
        collidingObject.center + Eigen::Vector3f(expandedHalfSize.x(), -expandedHalfSize.y(), 0),
        collidingObject.center + Eigen::Vector3f(-expandedHalfSize.x(), expandedHalfSize.y(), 0),
        collidingObject.center + Eigen::Vector3f(-expandedHalfSize.x(), -expandedHalfSize.y(), 0) };

    // Find the two closest corners to the robot
    std::sort(corners.begin(), corners.end(), [&robot](const Eigen::Vector3f& a, const Eigen::Vector3f& b)
        { return (a - robot.center).norm() < (b - robot.center).norm(); });

    for (const auto& cor : corners)
    {
        std::cout << "Closest corner:" << cor[0] << " y:" << cor[1] << " z:" << cor[2] << std::endl;
    }
    std::cout << "destination:" << destination[0] << " y:" << destination[1] << " z:" << destination[2] << std::endl;

    const float distA = (corners[0] - destination).norm();
    const float distB = (corners[1] - destination).norm();
    Eigen::Vector3f bestCorner;
    // if we are already on a corner we need to check the other 2 corner,
    // But if we are a distant object we have to only check the 2 closer to us  (otherwise we could collide ttrying to reach the farthest one)
    if ((corners[0] - robot.center).norm() <= MAPPER_THRESHOLD_DISTANCE_FOR_CORNER)
    {
        const float distC = (corners[2] - destination).norm();

        // Select the closest corner to the destination from the two nearest corners
        bestCorner = (distA < distB)
            ? (distA < distC ? corners[0] : corners[2])
            : (distB < distC ? corners[1] : corners[2]);
    }
    else // 2 corners
    {
        bestCorner = (distA < distB) ? corners[0] : corners[1];
    }
    return bestCorner;
}

/// @brief recursive function to get next position to reach toward the destination with detected object
/// this function if detecting an obstacle will reach the closest position to this object (around it)
/// then recursively call itself from this new position  to get the next pos
/// @param robot
/// @param destination
/// @param objects
/// @return
void Mapper::recursiveCalculateNextPathPositionToGoal(const RobotSpatialInfos& robot,
    const Eigen::Vector3f& destination,
    const std::vector<Object3D>& objects,
    std::vector<Eigen::Vector3f>& pathToFill)
{
    if (pathToFill.size() >= MAPPER_MAX_DEPTH_PATH_NUMBER_POINT)
    {
        std::cerr << TAG << "Pathfinding reached max recursion depth " << MAPPER_MAX_DEPTH_PATH_NUMBER_POINT << ", terminating pathfinding!" << std::endl;
        return;
    }

    std::vector<Object3D> filteredObjects;
    for (const auto& obj : objects)
    {
        if ((obj.center - robot.center).dot(destination - robot.center) > 0)
        {
            filteredObjects.push_back(obj);
        }
    }

    // Check for collisions
    Eigen::Vector3f nextPosition = destination;
    float closestCollisionDist = std::numeric_limits<float>::max();
    Object3D closestObject;
    bool foundCollision = false;

    for (const auto& obj : filteredObjects)
    {
        if (lineIntersectsAABB(robot, obj, destination))
        {
            float distance = (obj.center - robot.center).norm();
            if (distance < closestCollisionDist)
            {
                std::cout << "Collision detected for " << robot << ", And: " << obj << "in direction of detination: " << destination[0] << ",y:" << destination[1] << ",z:" << destination[2] << std::endl;
                closestCollisionDist = distance;
                closestObject = obj;
                foundCollision = true;
            }
        }
    }

    if (foundCollision)
    {
        auto nextPosition = Mapper::getCollidingNextPositionCloserBorderLogic(robot, closestObject, destination);

        std::cout << "From ob:" << static_cast<Object3D>(robot)
            << ", to destination x:" << destination[0] << " y:" << destination[1] << " z:" << destination[2] << std::endl;
        std::cout << "closestObject:" << static_cast<Object3D>(closestObject)
            << ", nextPosition x:" << nextPosition[0] << " y:" << nextPosition[1] << " z:" << nextPosition[2] << std::endl;

        RobotSpatialInfos updatedRobot = RobotSpatialInfos(robot);
        updatedRobot.center = nextPosition;

        if (updatedRobot.center != robot.center)
        {
            pathToFill.push_back(nextPosition);
            recursiveCalculateNextPathPositionToGoal(updatedRobot, destination, objects, pathToFill);
        }
        else
        {
            std::cerr << TAG << "ERROR: PATHFINDING entered infinite loop. UpdatedRobot:" << static_cast<Object3D>(updatedRobot)
                << ", robot:" << static_cast<Object3D>(robot) << std::endl;
        }
    }
    else
    {
        // No collision, go directly to the destination
        pathToFill.push_back(nextPosition);
    }
}

/// @brief This function will link previous detected objects with the new ones, each object will have it's id updated and
/// either matching the old one or having a unique one
/// @param refined_currentDetectedObject
/// @param refined_lastDetectedObject
/// @param robotMovement
void Mapper::linkDetectedObjects(
    std::vector<Object3D>& refined_currentDetectedObject, // ref as we update it
    const std::vector<Object3D>& refined_lastDetectedObject,
    const Eigen::Vector3f& robotMovement)
{
    // Maximum allowable distance to consider as the same object, in the future may be dependent of the robot movement
    const float maxDistanceThreshold = MAPPER_MARGING_DETECTION_THRESHOLD_MERGER;

    // Track matched objects
    std::vector<bool> matchedCurrent(refined_currentDetectedObject.size(), false);
    std::vector<bool> matchedLast(refined_lastDetectedObject.size(), false);
    for (size_t i = 0; i < refined_currentDetectedObject.size(); ++i)
    {
        float minDistance = maxDistanceThreshold;
        int bestMatchIdx = -1;
        for (size_t j = 0; j < refined_lastDetectedObject.size(); ++j)
        {
            if (matchedLast[j])
                continue; // Skip already matched last objects

            // Calculate the distance between current and last objects, so we can take the closest
            float distance = (refined_currentDetectedObject[i].center - (refined_lastDetectedObject[j].center + robotMovement)).norm();
            if (distance < minDistance)
            {
                minDistance = distance;
                bestMatchIdx = static_cast<int>(j);
            }
        }
        if (bestMatchIdx != -1)
        {
            refined_currentDetectedObject[i].id = refined_lastDetectedObject[bestMatchIdx].id;
            matchedCurrent[i] = true;
            matchedLast[bestMatchIdx] = true;
        }
        else
        {
            refined_currentDetectedObject[i].id = getObjectNewId();
        }
    }
}

// Getter for refinedCurrentDetectedObject
std::vector<Object3D> Mapper::getRefinedCurrentDetectedObject() {
    std::lock_guard<std::mutex> lock(_mutexDetectedObject);
    return _refinedCurrentDetectedObject;
}

// Getter for refinedLastDetectedObject
std::vector<Object3D> Mapper::getRefinedLastDetectedObject() {
    std::lock_guard<std::mutex> lock(_mutexDetectedObject);
    return _refinedLastDetectedObject;
}
