#include "Map.h"
#include "RAMCellManager.h"
#include "HardDriveCellManager.h"
#include <nabo/nabo.h>
#include <unordered_map>
#include "CellInfo.h"

struct CoordinateHashFunction : public std::unary_function<std::tuple<int, int, int>, std::size_t>
{
	std::size_t operator()(const std::tuple<int, int, int>& tuple) const
	{
		return std::hash<std::string>()(std::to_string(std::get<0>(tuple)) + std::to_string(std::get<1>(tuple)) + std::to_string(std::get<2>(tuple)));
	}
};

norlab_icp_mapper::Map::Map(const float& minDistNewPoint, const float& sensorMaxRange, const float& priorDynamic, const float& thresholdDynamic,
							const float& beamHalfAngle, const float& epsilonA, const float& epsilonD, const float& alpha, const float& beta, const bool& is3D,
							const bool& isOnline, const bool& computeProbDynamic, const bool& saveCellsOnHardDrive, PM::ICPSequence& icp,
							std::mutex& icpMapLock) :
		minDistNewPoint(minDistNewPoint),
		sensorMaxRange(sensorMaxRange),
		priorDynamic(priorDynamic),
		thresholdDynamic(thresholdDynamic),
		beamHalfAngle(beamHalfAngle),
		epsilonA(epsilonA),
		epsilonD(epsilonD),
		alpha(alpha),
		beta(beta),
		is3D(is3D),
		isOnline(isOnline),
		computeProbDynamic(computeProbDynamic),
		icp(icp),
		icpMapLock(icpMapLock),
		transformation(PM::get().TransformationRegistrar.create("RigidTransformation")),
		newLocalPointCloudAvailable(false),
		localPointCloudEmpty(true),
		firstPoseUpdate(true),
		updateThreadLooping(true),
		pose(PM::TransformationParameters::Identity(is3D ? 4 : 3, is3D ? 4 : 3))
{
	if(saveCellsOnHardDrive)
	{
		cellManager = std::unique_ptr<CellManager>(new HardDriveCellManager());
	}
	else
	{
		cellManager = std::unique_ptr<CellManager>(new RAMCellManager());
	}

	if(isOnline)
	{
		updateThread = std::thread(&Map::updateThreadFunction, this);
	}
}

void norlab_icp_mapper::Map::updateThreadFunction()
{
	while(updateThreadLooping.load())
	{
		updateListLock.lock();
		bool isUpdateListEmpty = updateList.empty();
		updateListLock.unlock();

		if(!isUpdateListEmpty)
		{
			updateListLock.lock();
			Update update = updateList.front();
			updateList.pop_front();
			updateListLock.unlock();

			applyUpdate(update);
		}
		else
		{
			std::this_thread::sleep_for(std::chrono::duration<float>(0.01));
		}
	}
}

void norlab_icp_mapper::Map::applyUpdate(const Update& update)
{
	if(update.load)
	{
		loadCells(update.startRow, update.endRow, update.startColumn, update.endColumn, update.startAisle, update.endAisle);
	}
	else
	{
		unloadCells(update.startRow, update.endRow, update.startColumn, update.endColumn, update.startAisle, update.endAisle);
	}
}

void norlab_icp_mapper::Map::loadCells(int startRow, int endRow, int startColumn, int endColumn, int startAisle, int endAisle)
{
	localPointCloudLock.lock();
	std::unordered_set<CellInfo> currentlyLoadedCellInfos = loadedCellInfos;
	localPointCloudLock.unlock();
	poseLock.lock();
	PM::TransformationParameters currentPose = pose;
	poseLock.unlock();
	int positionColumn = is3D ? 3 : 2;

	std::unordered_set<CellInfo> newCellInfos;
	PM::DataPoints newCells;
	std::vector<int> rowIndexes = getOrderedIndexes(startRow, endRow, toGridCoordinate(currentPose(0, positionColumn)));
	for(const int& i: rowIndexes)
	{
		std::vector<int> columnIndexes = getOrderedIndexes(startColumn, endColumn, toGridCoordinate(currentPose(1, positionColumn)));
		for(const int& j: columnIndexes)
		{
			std::vector<int> aisleIndexes = {0};
			if(is3D)
			{
				aisleIndexes = getOrderedIndexes(startAisle, endAisle, toGridCoordinate(currentPose(2, positionColumn)));
			}
			for(const int& k: aisleIndexes)
			{
				int depth = computeDepthOfCell(currentlyLoadedCellInfos, i, j, k);
				cellManagerLock.lock();
				std::pair<CellInfo, PM::DataPoints> cell = cellManager->retrieveCell(i, j, k, depth);
				cellManagerLock.unlock();

				if(cell.first.depth == CellManager::INVALID_CELL_DEPTH)
				{
					cell.first.depth = depth;
				}
				else
				{
					if(newCells.getNbPoints() == 0)
					{
						newCells = cell.second;
					}
					else
					{
						newCells.concatenate(cell.second);
					}
				}
				currentlyLoadedCellInfos.insert(cell.first);
				newCellInfos.insert(cell.first);
			}
		}
	}

	localPointCloudLock.lock();
	if(newCells.getNbPoints() > 0)
	{
		localPointCloud.concatenate(newCells);

		icpMapLock.lock();
		icp.setMap(localPointCloud);
		icpMapLock.unlock();

		localPointCloudEmpty.store(false);
		newLocalPointCloudAvailable = true;
	}
	loadedCellInfos.insert(newCellInfos.begin(), newCellInfos.end());
	localPointCloudLock.unlock();
}

std::vector<int> norlab_icp_mapper::Map::getOrderedIndexes(int lowIndex, int highIndex, int currentIndex) const
{
	std::vector<int> orderedIndexes;
	if(highIndex <= currentIndex)
	{
		for(int i = highIndex; i >= lowIndex; --i)
		{
			orderedIndexes.push_back(i);
		}
	}
	else if(lowIndex >= currentIndex)
	{
		for(int i = lowIndex; i <= highIndex; ++i)
		{
			orderedIndexes.push_back(i);
		}
	}
	else
	{
		for(int i = currentIndex; i >= lowIndex; --i)
		{
			orderedIndexes.push_back(i);
		}
		for(int i = currentIndex + 1; i <= highIndex; ++i)
		{
			orderedIndexes.push_back(i);
		}
	}
	return orderedIndexes;
}

int norlab_icp_mapper::Map::computeDepthOfCell(const std::unordered_set<CellInfo>& currentlyLoadedCellInfos, int row, int column, int aisle) const
{
	if(currentlyLoadedCellInfos.empty())
	{
		return 0;
	}

	int minCellDepth = std::numeric_limits<int>::max();
	for(const CellInfo& cellInfo: currentlyLoadedCellInfos)
	{
		int dx = std::abs(row - cellInfo.row);
		int dy = std::abs(column - cellInfo.column);
		int dz = std::abs(aisle - cellInfo.aisle);
		int gridDistance = std::max(std::max(dx, dy), dz);
		if(cellInfo.depth + gridDistance < minCellDepth)
		{
			minCellDepth = cellInfo.depth + gridDistance;
		}
	}
	return minCellDepth;
}

void norlab_icp_mapper::Map::unloadCells(int startRow, int endRow, int startColumn, int endColumn, int startAisle, int endAisle)
{
	if(!is3D)
	{
		startAisle = 0;
		endAisle = 0;
	}

	float startX = toInferiorWorldCoordinate(startRow);
	float endX = toSuperiorWorldCoordinate(endRow);
	float startY = toInferiorWorldCoordinate(startColumn);
	float endY = toSuperiorWorldCoordinate(endColumn);
	float startZ = toInferiorWorldCoordinate(startAisle);
	float endZ = toSuperiorWorldCoordinate(endAisle);

	int localPointCloudNbPoints = 0;
	int oldCellsNbPoints = 0;

	localPointCloudLock.lock();
	PM::DataPoints oldCells = localPointCloud.createSimilarEmpty();
	for(int i = 0; i < localPointCloud.features.cols(); i++)
	{
		if(localPointCloud.features(0, i) >= startX && localPointCloud.features(0, i) < endX && localPointCloud.features(1, i) >= startY &&
		   localPointCloud.features(1, i) < endY && localPointCloud.features(2, i) >= startZ && localPointCloud.features(2, i) < endZ)
		{
			oldCells.setColFrom(oldCellsNbPoints, localPointCloud, i);
			oldCellsNbPoints++;
		}
		else
		{
			localPointCloud.setColFrom(localPointCloudNbPoints, localPointCloud, i);
			localPointCloudNbPoints++;
		}
	}
	localPointCloud.conservativeResize(localPointCloudNbPoints);

	icpMapLock.lock();
	icp.setMap(localPointCloud);
	icpMapLock.unlock();

	std::unordered_map<std::tuple<int, int, int>, int, CoordinateHashFunction> oldCellDepths;
	if(loadedCellInfos.empty() && oldCells.descriptorExists("depths"))
	{
		const auto& cellDepthView = oldCells.getDescriptorViewByName("depths");
		for(int i = 0; i < oldCells.features.cols(); i++)
		{
			int row = toGridCoordinate(oldCells.features(0, i));
			int column = toGridCoordinate(oldCells.features(1, i));
			int aisle = 0;
			if(is3D)
			{
				aisle = toGridCoordinate(oldCells.features(2, i));
			}
			oldCellDepths[std::make_tuple(row, column, aisle)] = cellDepthView(0, i);
		}
		localPointCloud.removeDescriptor("depths");
		oldCells.removeDescriptor("depths");
	}
	else
	{
		std::unordered_set<CellInfo>::const_iterator cellInfoIterator = loadedCellInfos.begin();
		while(cellInfoIterator != loadedCellInfos.end())
		{
			if(cellInfoIterator->row >= startRow && cellInfoIterator->row <= endRow && cellInfoIterator->column >= startColumn &&
			   cellInfoIterator->column <= endColumn && cellInfoIterator->aisle >= startAisle && cellInfoIterator->aisle <= endAisle)
			{
				oldCellDepths[std::make_tuple(cellInfoIterator->row, cellInfoIterator->column, cellInfoIterator->aisle)] = cellInfoIterator->depth;
				cellInfoIterator = loadedCellInfos.erase(cellInfoIterator);
			}
			else
			{
				++cellInfoIterator;
			}
		}
	}

	localPointCloudEmpty.store(localPointCloud.getNbPoints() == 0);
	newLocalPointCloudAvailable = true;

	localPointCloudLock.unlock();

	oldCells.conservativeResize(oldCellsNbPoints);

	std::unordered_map<CellInfo, PM::DataPoints> cells;
	std::unordered_map<CellInfo, int> cellPointCounts;
	for(int i = 0; i < oldCells.getNbPoints(); i++)
	{
		int row = toGridCoordinate(oldCells.features(0, i));
		int column = toGridCoordinate(oldCells.features(1, i));
		int aisle = toGridCoordinate(oldCells.features(2, i));
		int depth = oldCellDepths[std::make_tuple(row, column, aisle)];
		CellInfo cellInfo(row, column, aisle, depth);

		if(cells[cellInfo].getNbPoints() == 0)
		{
			cells[cellInfo] = oldCells.createSimilarEmpty();
		}

		cells[cellInfo].setColFrom(cellPointCounts[cellInfo], oldCells, i);
		cellPointCounts[cellInfo]++;
	}
	for(auto& cell: cells)
	{
		cell.second.conservativeResize(cellPointCounts[cell.first]);
		cellManagerLock.lock();
		cellManager->saveCell(cell.first, cell.second);
		cellManagerLock.unlock();
	}
}

float norlab_icp_mapper::Map::toInferiorWorldCoordinate(const int& gridCoordinate) const
{
	return gridCoordinate * CELL_SIZE;
}

float norlab_icp_mapper::Map::toSuperiorWorldCoordinate(const int& gridCoordinate) const
{
	return (gridCoordinate + 1) * CELL_SIZE;
}

int norlab_icp_mapper::Map::toGridCoordinate(const float& worldCoordinate) const
{
	return std::floor(worldCoordinate / CELL_SIZE);
}

norlab_icp_mapper::Map::~Map()
{
	if(isOnline)
	{
		updateThreadLooping.store(false);
		updateThread.join();
	}
}

void norlab_icp_mapper::Map::updatePose(const PM::TransformationParameters& newPose)
{
	poseLock.lock();
	pose = newPose;
	poseLock.unlock();

	int positionColumn = is3D ? 3 : 2;
	if(firstPoseUpdate.load())
	{
		inferiorRowLastUpdateIndex = toInferiorGridCoordinate(newPose(0, positionColumn), sensorMaxRange);
		superiorRowLastUpdateIndex = toSuperiorGridCoordinate(newPose(0, positionColumn), sensorMaxRange);
		inferiorColumnLastUpdateIndex = toInferiorGridCoordinate(newPose(1, positionColumn), sensorMaxRange);
		superiorColumnLastUpdateIndex = toSuperiorGridCoordinate(newPose(1, positionColumn), sensorMaxRange);
		if(is3D)
		{
			inferiorAisleLastUpdateIndex = toInferiorGridCoordinate(newPose(2, positionColumn), sensorMaxRange);
			superiorAisleLastUpdateIndex = toSuperiorGridCoordinate(newPose(2, positionColumn), sensorMaxRange);
		}

		cellManagerLock.lock();
		cellManager->clearAllCells();
		cellManagerLock.unlock();
		localPointCloudLock.lock();
		loadedCellInfos.clear();
		localPointCloudLock.unlock();

		unloadCells(getMinGridCoordinate(), getMaxGridCoordinate(), getMinGridCoordinate(),
					getMaxGridCoordinate(), getMinGridCoordinate(), getMaxGridCoordinate());
		loadCells(inferiorRowLastUpdateIndex - BUFFER_SIZE, superiorRowLastUpdateIndex + BUFFER_SIZE, inferiorColumnLastUpdateIndex - BUFFER_SIZE,
				  superiorColumnLastUpdateIndex + BUFFER_SIZE, inferiorAisleLastUpdateIndex - BUFFER_SIZE, superiorAisleLastUpdateIndex + BUFFER_SIZE);

		firstPoseUpdate.store(false);
	}
	else
	{
		// manage cells in the back
		if(std::abs(toInferiorGridCoordinate(newPose(0, positionColumn), sensorMaxRange) - inferiorRowLastUpdateIndex) >= 2)
		{
			// move back
			if(toInferiorGridCoordinate(newPose(0, positionColumn), sensorMaxRange) < inferiorRowLastUpdateIndex)
			{
				int nbRows = inferiorRowLastUpdateIndex - toInferiorGridCoordinate(newPose(0, positionColumn), sensorMaxRange);
				int startRow = toInferiorGridCoordinate(newPose(0, positionColumn), sensorMaxRange) - BUFFER_SIZE;
				int endRow = toInferiorGridCoordinate(newPose(0, positionColumn), sensorMaxRange) - BUFFER_SIZE + nbRows - 1;
				int startColumn = inferiorColumnLastUpdateIndex - BUFFER_SIZE;
				int endColumn = superiorColumnLastUpdateIndex + BUFFER_SIZE;
				int startAisle = inferiorAisleLastUpdateIndex - BUFFER_SIZE;
				int endAisle = superiorAisleLastUpdateIndex + BUFFER_SIZE;
				scheduleUpdate(Update{startRow, endRow, startColumn, endColumn, startAisle, endAisle, true});
			}
			// move front
			if(toInferiorGridCoordinate(newPose(0, positionColumn), sensorMaxRange) > inferiorRowLastUpdateIndex)
			{
				int nbRows = toInferiorGridCoordinate(newPose(0, positionColumn), sensorMaxRange) - inferiorRowLastUpdateIndex;
				int startRow = inferiorRowLastUpdateIndex - BUFFER_SIZE;
				int endRow = inferiorRowLastUpdateIndex - BUFFER_SIZE + nbRows - 1;
				int startColumn = inferiorColumnLastUpdateIndex - BUFFER_SIZE;
				int endColumn = superiorColumnLastUpdateIndex + BUFFER_SIZE;
				int startAisle = inferiorAisleLastUpdateIndex - BUFFER_SIZE;
				int endAisle = superiorAisleLastUpdateIndex + BUFFER_SIZE;
				scheduleUpdate(Update{startRow, endRow, startColumn, endColumn, startAisle, endAisle, false});
			}
			inferiorRowLastUpdateIndex = toInferiorGridCoordinate(newPose(0, positionColumn), sensorMaxRange);
		}

		// manage cells in front
		if(std::abs(toSuperiorGridCoordinate(newPose(0, positionColumn), sensorMaxRange) - superiorRowLastUpdateIndex) >= 2)
		{
			// move back
			if(toSuperiorGridCoordinate(newPose(0, positionColumn), sensorMaxRange) < superiorRowLastUpdateIndex)
			{
				int nbRows = superiorRowLastUpdateIndex - toSuperiorGridCoordinate(newPose(0, positionColumn), sensorMaxRange);
				int startRow = superiorRowLastUpdateIndex + BUFFER_SIZE - nbRows + 1;
				int endRow = superiorRowLastUpdateIndex + BUFFER_SIZE;
				int startColumn = inferiorColumnLastUpdateIndex - BUFFER_SIZE;
				int endColumn = superiorColumnLastUpdateIndex + BUFFER_SIZE;
				int startAisle = inferiorAisleLastUpdateIndex - BUFFER_SIZE;
				int endAisle = superiorAisleLastUpdateIndex + BUFFER_SIZE;
				scheduleUpdate(Update{startRow, endRow, startColumn, endColumn, startAisle, endAisle, false});
			}
			// move front
			if(toSuperiorGridCoordinate(newPose(0, positionColumn), sensorMaxRange) > superiorRowLastUpdateIndex)
			{
				int nbRows = toSuperiorGridCoordinate(newPose(0, positionColumn), sensorMaxRange) - superiorRowLastUpdateIndex;
				int startRow = toSuperiorGridCoordinate(newPose(0, positionColumn), sensorMaxRange) + BUFFER_SIZE - nbRows + 1;
				int endRow = toSuperiorGridCoordinate(newPose(0, positionColumn), sensorMaxRange) + BUFFER_SIZE;
				int startColumn = inferiorColumnLastUpdateIndex - BUFFER_SIZE;
				int endColumn = superiorColumnLastUpdateIndex + BUFFER_SIZE;
				int startAisle = inferiorAisleLastUpdateIndex - BUFFER_SIZE;
				int endAisle = superiorAisleLastUpdateIndex + BUFFER_SIZE;
				scheduleUpdate(Update{startRow, endRow, startColumn, endColumn, startAisle, endAisle, true});
			}
			superiorRowLastUpdateIndex = toSuperiorGridCoordinate(newPose(0, positionColumn), sensorMaxRange);
		}

		// update cells to the right
		if(std::abs(toInferiorGridCoordinate(newPose(1, positionColumn), sensorMaxRange) - inferiorColumnLastUpdateIndex) >= 2)
		{
			// move right
			if(toInferiorGridCoordinate(newPose(1, positionColumn), sensorMaxRange) < inferiorColumnLastUpdateIndex)
			{
				int startRow = inferiorRowLastUpdateIndex - BUFFER_SIZE;
				int endRow = superiorRowLastUpdateIndex + BUFFER_SIZE;
				int nbColumns = inferiorColumnLastUpdateIndex - toInferiorGridCoordinate(newPose(1, positionColumn), sensorMaxRange);
				int startColumn = toInferiorGridCoordinate(newPose(1, positionColumn), sensorMaxRange) - BUFFER_SIZE;
				int endColumn = toInferiorGridCoordinate(newPose(1, positionColumn), sensorMaxRange) - BUFFER_SIZE + nbColumns - 1;
				int startAisle = inferiorAisleLastUpdateIndex - BUFFER_SIZE;
				int endAisle = superiorAisleLastUpdateIndex + BUFFER_SIZE;
				scheduleUpdate(Update{startRow, endRow, startColumn, endColumn, startAisle, endAisle, true});
			}
			// move left
			if(toInferiorGridCoordinate(newPose(1, positionColumn), sensorMaxRange) > inferiorColumnLastUpdateIndex)
			{
				int startRow = inferiorRowLastUpdateIndex - BUFFER_SIZE;
				int endRow = superiorRowLastUpdateIndex + BUFFER_SIZE;
				int nbColumns = toInferiorGridCoordinate(newPose(1, positionColumn), sensorMaxRange) - inferiorColumnLastUpdateIndex;
				int startColumn = inferiorColumnLastUpdateIndex - BUFFER_SIZE;
				int endColumn = inferiorColumnLastUpdateIndex - BUFFER_SIZE + nbColumns - 1;
				int startAisle = inferiorAisleLastUpdateIndex - BUFFER_SIZE;
				int endAisle = superiorAisleLastUpdateIndex + BUFFER_SIZE;
				scheduleUpdate(Update{startRow, endRow, startColumn, endColumn, startAisle, endAisle, false});
			}
			inferiorColumnLastUpdateIndex = toInferiorGridCoordinate(newPose(1, positionColumn), sensorMaxRange);
		}

		// update cells to the left
		if(std::abs(toSuperiorGridCoordinate(newPose(1, positionColumn), sensorMaxRange) - superiorColumnLastUpdateIndex) >= 2)
		{
			// move right
			if(toSuperiorGridCoordinate(newPose(1, positionColumn), sensorMaxRange) < superiorColumnLastUpdateIndex)
			{
				int startRow = inferiorRowLastUpdateIndex - BUFFER_SIZE;
				int endRow = superiorRowLastUpdateIndex + BUFFER_SIZE;
				int nbColumns = superiorColumnLastUpdateIndex - toSuperiorGridCoordinate(newPose(1, positionColumn), sensorMaxRange);
				int startColumn = superiorColumnLastUpdateIndex + BUFFER_SIZE - nbColumns + 1;
				int endColumn = superiorColumnLastUpdateIndex + BUFFER_SIZE;
				int startAisle = inferiorAisleLastUpdateIndex - BUFFER_SIZE;
				int endAisle = superiorAisleLastUpdateIndex + BUFFER_SIZE;
				scheduleUpdate(Update{startRow, endRow, startColumn, endColumn, startAisle, endAisle, false});
			}
			// move left
			if(toSuperiorGridCoordinate(newPose(1, positionColumn), sensorMaxRange) > superiorColumnLastUpdateIndex)
			{
				int startRow = inferiorRowLastUpdateIndex - BUFFER_SIZE;
				int endRow = superiorRowLastUpdateIndex + BUFFER_SIZE;
				int nbColumns = toSuperiorGridCoordinate(newPose(1, positionColumn), sensorMaxRange) - superiorColumnLastUpdateIndex;
				int startColumn = toSuperiorGridCoordinate(newPose(1, positionColumn), sensorMaxRange) + BUFFER_SIZE - nbColumns + 1;
				int endColumn = toSuperiorGridCoordinate(newPose(1, positionColumn), sensorMaxRange) + BUFFER_SIZE;
				int startAisle = inferiorAisleLastUpdateIndex - BUFFER_SIZE;
				int endAisle = superiorAisleLastUpdateIndex + BUFFER_SIZE;
				scheduleUpdate(Update{startRow, endRow, startColumn, endColumn, startAisle, endAisle, true});
			}
			superiorColumnLastUpdateIndex = toSuperiorGridCoordinate(newPose(1, positionColumn), sensorMaxRange);
		}

		if(is3D)
		{
			// update cells below
			if(std::abs(toInferiorGridCoordinate(newPose(2, positionColumn), sensorMaxRange) - inferiorAisleLastUpdateIndex) >= 2)
			{
				// move down
				if(toInferiorGridCoordinate(newPose(2, positionColumn), sensorMaxRange) < inferiorAisleLastUpdateIndex)
				{
					int startRow = inferiorRowLastUpdateIndex - BUFFER_SIZE;
					int endRow = superiorRowLastUpdateIndex + BUFFER_SIZE;
					int startColumn = inferiorColumnLastUpdateIndex - BUFFER_SIZE;
					int endColumn = superiorColumnLastUpdateIndex + BUFFER_SIZE;
					int nbAisles = inferiorAisleLastUpdateIndex - toInferiorGridCoordinate(newPose(2, positionColumn), sensorMaxRange);
					int startAisle = toInferiorGridCoordinate(newPose(2, positionColumn), sensorMaxRange) - BUFFER_SIZE;
					int endAisle = toInferiorGridCoordinate(newPose(2, positionColumn), sensorMaxRange) - BUFFER_SIZE + nbAisles - 1;
					scheduleUpdate(Update{startRow, endRow, startColumn, endColumn, startAisle, endAisle, true});
				}
				// move up
				if(toInferiorGridCoordinate(newPose(2, positionColumn), sensorMaxRange) > inferiorAisleLastUpdateIndex)
				{
					int startRow = inferiorRowLastUpdateIndex - BUFFER_SIZE;
					int endRow = superiorRowLastUpdateIndex + BUFFER_SIZE;
					int startColumn = inferiorColumnLastUpdateIndex - BUFFER_SIZE;
					int endColumn = superiorColumnLastUpdateIndex + BUFFER_SIZE;
					int nbAisles = toInferiorGridCoordinate(newPose(2, positionColumn), sensorMaxRange) - inferiorAisleLastUpdateIndex;
					int startAisle = inferiorAisleLastUpdateIndex - BUFFER_SIZE;
					int endAisle = inferiorAisleLastUpdateIndex - BUFFER_SIZE + nbAisles - 1;
					scheduleUpdate(Update{startRow, endRow, startColumn, endColumn, startAisle, endAisle, false});
				}
				inferiorAisleLastUpdateIndex = toInferiorGridCoordinate(newPose(2, positionColumn), sensorMaxRange);
			}

			// update cells above
			if(std::abs(toSuperiorGridCoordinate(newPose(2, positionColumn), sensorMaxRange) - superiorAisleLastUpdateIndex) >= 2)
			{
				// move down
				if(toSuperiorGridCoordinate(newPose(2, positionColumn), sensorMaxRange) < superiorAisleLastUpdateIndex)
				{
					int startRow = inferiorRowLastUpdateIndex - BUFFER_SIZE;
					int endRow = superiorRowLastUpdateIndex + BUFFER_SIZE;
					int startColumn = inferiorColumnLastUpdateIndex - BUFFER_SIZE;
					int endColumn = superiorColumnLastUpdateIndex + BUFFER_SIZE;
					int nbAisles = superiorAisleLastUpdateIndex - toSuperiorGridCoordinate(newPose(2, positionColumn), sensorMaxRange);
					int startAisle = superiorAisleLastUpdateIndex + BUFFER_SIZE - nbAisles + 1;
					int endAisle = superiorAisleLastUpdateIndex + BUFFER_SIZE;
					scheduleUpdate(Update{startRow, endRow, startColumn, endColumn, startAisle, endAisle, false});
				}
				// move up
				if(toSuperiorGridCoordinate(newPose(2, positionColumn), sensorMaxRange) > superiorAisleLastUpdateIndex)
				{
					int startRow = inferiorRowLastUpdateIndex - BUFFER_SIZE;
					int endRow = superiorRowLastUpdateIndex + BUFFER_SIZE;
					int startColumn = inferiorColumnLastUpdateIndex - BUFFER_SIZE;
					int endColumn = superiorColumnLastUpdateIndex + BUFFER_SIZE;
					int nbAisles = toSuperiorGridCoordinate(newPose(2, positionColumn), sensorMaxRange) - superiorAisleLastUpdateIndex;
					int startAisle = toSuperiorGridCoordinate(newPose(2, positionColumn), sensorMaxRange) + BUFFER_SIZE - nbAisles + 1;
					int endAisle = toSuperiorGridCoordinate(newPose(2, positionColumn), sensorMaxRange) + BUFFER_SIZE;
					scheduleUpdate(Update{startRow, endRow, startColumn, endColumn, startAisle, endAisle, true});
				}
				superiorAisleLastUpdateIndex = toSuperiorGridCoordinate(newPose(2, positionColumn), sensorMaxRange);
			}
		}
	}
}

int norlab_icp_mapper::Map::getMinGridCoordinate() const
{
	return std::numeric_limits<int>::lowest();
}

int norlab_icp_mapper::Map::getMaxGridCoordinate() const
{
	return std::numeric_limits<int>::max() - 1;
}

int norlab_icp_mapper::Map::toInferiorGridCoordinate(const float& worldCoordinate, const float& range) const
{
	return std::ceil(((worldCoordinate - range) / CELL_SIZE) - 1.0);
}

int norlab_icp_mapper::Map::toSuperiorGridCoordinate(const float& worldCoordinate, const float& range) const
{
	return std::floor((worldCoordinate + range) / CELL_SIZE);
}

void norlab_icp_mapper::Map::scheduleUpdate(const Update& update)
{
	if(isOnline)
	{
		updateListLock.lock();
		updateList.push_back(update);
		updateListLock.unlock();
	}
	else
	{
		applyUpdate(update);
	}
}

norlab_icp_mapper::Map::PM::DataPoints norlab_icp_mapper::Map::getLocalPointCloud()
{
	std::lock_guard<std::mutex> lock(localPointCloudLock);
	return localPointCloud;
}

void norlab_icp_mapper::Map::updateLocalPointCloud(PM::DataPoints input, PM::TransformationParameters pose, PM::DataPointsFilters postFilters)
{
	if(computeProbDynamic)
	{
		input.addDescriptor("probabilityDynamic", PM::Matrix::Constant(1, input.features.cols(), priorDynamic));
	}

	localPointCloudLock.lock();
	if(localPointCloudEmpty.load())
	{
		localPointCloud = input;
	}
	else
	{
		if(computeProbDynamic)
		{
			computeProbabilityOfPointsBeingDynamic(input, localPointCloud, pose);
		}

		PM::DataPoints inputPointsToKeep = retrievePointsFurtherThanMinDistNewPoint(input, localPointCloud, pose);
		localPointCloud.concatenate(inputPointsToKeep);
	}

	PM::DataPoints localPointCloudInSensorFrame = transformation->compute(localPointCloud, pose.inverse());
	postFilters.apply(localPointCloudInSensorFrame);
	localPointCloud = transformation->compute(localPointCloudInSensorFrame, pose);

	icpMapLock.lock();
	icp.setMap(localPointCloud);
	icpMapLock.unlock();

	localPointCloudEmpty.store(localPointCloud.getNbPoints() == 0);
	newLocalPointCloudAvailable = true;
	localPointCloudLock.unlock();
}

void norlab_icp_mapper::Map::computeProbabilityOfPointsBeingDynamic(const PM::DataPoints& input, PM::DataPoints& currentLocalPointCloud,
																	const PM::TransformationParameters& pose) const
{
	typedef Nabo::NearestNeighbourSearch<float> NNS;
	const float eps = 0.0001;

	PM::DataPoints inputInSensorFrame = transformation->compute(input, pose.inverse());

	PM::Matrix inputInSensorFrameRadii;
	PM::Matrix inputInSensorFrameAngles;
	convertToSphericalCoordinates(inputInSensorFrame, inputInSensorFrameRadii, inputInSensorFrameAngles);

	PM::DataPoints currentLocalPointCloudInSensorFrame = transformation->compute(currentLocalPointCloud, pose.inverse());
	PM::Matrix globalId(1, currentLocalPointCloud.getNbPoints());
	int nbPointsWithinSensorMaxRange = 0;
	for(int i = 0; i < currentLocalPointCloud.getNbPoints(); i++)
	{
		if(currentLocalPointCloudInSensorFrame.features.col(i).head(currentLocalPointCloudInSensorFrame.getEuclideanDim()).norm() < sensorMaxRange)
		{
			currentLocalPointCloudInSensorFrame.setColFrom(nbPointsWithinSensorMaxRange, currentLocalPointCloudInSensorFrame, i);
			globalId(0, nbPointsWithinSensorMaxRange) = i;
			nbPointsWithinSensorMaxRange++;
		}
	}
	currentLocalPointCloudInSensorFrame.conservativeResize(nbPointsWithinSensorMaxRange);

	PM::Matrix currentLocalPointCloudInSensorFrameRadii;
	PM::Matrix currentLocalPointCloudInSensorFrameAngles;
	convertToSphericalCoordinates(currentLocalPointCloudInSensorFrame, currentLocalPointCloudInSensorFrameRadii, currentLocalPointCloudInSensorFrameAngles);

	std::shared_ptr<NNS> nns = std::shared_ptr<NNS>(NNS::create(inputInSensorFrameAngles));
	PM::Matches::Dists dists(1, currentLocalPointCloudInSensorFrame.getNbPoints());
	PM::Matches::Ids ids(1, currentLocalPointCloudInSensorFrame.getNbPoints());
	nns->knn(currentLocalPointCloudInSensorFrameAngles, ids, dists, 1, 0, NNS::ALLOW_SELF_MATCH, 2 * beamHalfAngle);

	PM::DataPoints::View viewOnProbabilityDynamic = currentLocalPointCloud.getDescriptorViewByName("probabilityDynamic");
	PM::DataPoints::View viewOnNormals = currentLocalPointCloudInSensorFrame.getDescriptorViewByName("normals");
	for(int i = 0; i < currentLocalPointCloudInSensorFrame.getNbPoints(); i++)
	{
		if(dists(i) != std::numeric_limits<float>::infinity())
		{
			const int inputPointId = ids(0, i);
			const int localPointCloudPointId = globalId(0, i);

			const Eigen::VectorXf inputPoint = inputInSensorFrame.features.col(inputPointId).head(inputInSensorFrame.getEuclideanDim());
			const Eigen::VectorXf
					localPointCloudPoint = currentLocalPointCloudInSensorFrame.features.col(i).head(currentLocalPointCloudInSensorFrame.getEuclideanDim());
			const float delta = (inputPoint - localPointCloudPoint).norm();
			const float d_max = epsilonA * inputPoint.norm();

			const Eigen::VectorXf localPointCloudPointNormal = viewOnNormals.col(i);

			const float w_v = eps + (1. - eps) * fabs(localPointCloudPointNormal.dot(localPointCloudPoint.normalized()));
			const float w_d1 = eps + (1. - eps) * (1. - sqrt(dists(i)) / (2 * beamHalfAngle));

			const float offset = delta - epsilonD;
			float w_d2 = 1.;
			if(delta < epsilonD || localPointCloudPoint.norm() > inputPoint.norm())
			{
				w_d2 = eps;
			}
			else
			{
				if(offset < d_max)
				{
					w_d2 = eps + (1 - eps) * offset / d_max;
				}
			}

			float w_p2 = eps;
			if(delta < epsilonD)
			{
				w_p2 = 1;
			}
			else
			{
				if(offset < d_max)
				{
					w_p2 = eps + (1. - eps) * (1. - offset / d_max);
				}
			}

			if((inputPoint.norm() + epsilonD + d_max) >= localPointCloudPoint.norm())
			{
				const float lastDyn = viewOnProbabilityDynamic(0, localPointCloudPointId);

				const float c1 = (1 - (w_v * w_d1));
				const float c2 = w_v * w_d1;

				float probDynamic;
				float probStatic;
				if(lastDyn < thresholdDynamic)
				{
					probDynamic = c1 * lastDyn + c2 * w_d2 * ((1 - alpha) * (1 - lastDyn) + beta * lastDyn);
					probStatic = c1 * (1 - lastDyn) + c2 * w_p2 * (alpha * (1 - lastDyn) + (1 - beta) * lastDyn);
				}
				else
				{
					probDynamic = 1 - eps;
					probStatic = eps;
				}

				viewOnProbabilityDynamic(0, localPointCloudPointId) = probDynamic / (probDynamic + probStatic);
			}
		}
	}
}

norlab_icp_mapper::Map::PM::DataPoints norlab_icp_mapper::Map::retrievePointsFurtherThanMinDistNewPoint(const PM::DataPoints& input,
																										const PM::DataPoints& currentLocalPointCloud,
																										const PM::TransformationParameters& pose) const
{
	typedef Nabo::NearestNeighbourSearch<float> NNS;

	PM::Matches matches(PM::Matches::Dists(1, input.getNbPoints()), PM::Matches::Ids(1, input.getNbPoints()));
	std::shared_ptr<NNS> nns = std::shared_ptr<NNS>(NNS::create(currentLocalPointCloud.features, currentLocalPointCloud.features.rows() - 1,
																NNS::KDTREE_LINEAR_HEAP, NNS::TOUCH_STATISTICS));

	nns->knn(input.features, matches.ids, matches.dists, 1, 0);

	int goodPointCount = 0;
	PM::DataPoints goodPoints(input.createSimilarEmpty());
	for(int i = 0; i < input.getNbPoints(); ++i)
	{
		if(matches.dists(i) >= std::pow(minDistNewPoint, 2))
		{
			goodPoints.setColFrom(goodPointCount, input, i);
			goodPointCount++;
		}
	}
	goodPoints.conservativeResize(goodPointCount);

	return goodPoints;
}

void norlab_icp_mapper::Map::convertToSphericalCoordinates(const PM::DataPoints& points, PM::Matrix& radii, PM::Matrix& angles) const
{
	radii = points.features.topRows(points.getEuclideanDim()).colwise().norm();
	angles = PM::Matrix(2, points.getNbPoints());

	for(int i = 0; i < points.getNbPoints(); i++)
	{
		angles(0, i) = 0;
		if(is3D)
		{
			const float ratio = points.features(2, i) / radii(0, i);
			angles(0, i) = asin(ratio);
		}
		angles(1, i) = atan2(points.features(1, i), points.features(0, i));
	}
}

bool norlab_icp_mapper::Map::getNewLocalPointCloud(PM::DataPoints& localPointCloudOut)
{
	bool localPointCloudReturned = false;

	localPointCloudLock.lock();
	if(newLocalPointCloudAvailable)
	{
		localPointCloudOut = localPointCloud;
		newLocalPointCloudAvailable = false;
		localPointCloudReturned = true;
	}
	localPointCloudLock.unlock();

	return localPointCloudReturned;
}

norlab_icp_mapper::Map::PM::DataPoints norlab_icp_mapper::Map::getGlobalPointCloud()
{
	localPointCloudLock.lock();
	PM::DataPoints globalPointCloud = localPointCloud;
	std::unordered_set<CellInfo> currentLoadedCellInfos = loadedCellInfos;
	localPointCloudLock.unlock();

	std::unordered_map<std::tuple<int, int, int>, int, CoordinateHashFunction> cellDepths;
	std::unordered_set<CellInfo>::const_iterator cellInfoIterator = currentLoadedCellInfos.begin();
	while(cellInfoIterator != currentLoadedCellInfos.end())
	{
		cellDepths[std::make_tuple(cellInfoIterator->row, cellInfoIterator->column, cellInfoIterator->aisle)] = cellInfoIterator->depth;
		++cellInfoIterator;
	}
	PM::Matrix depths = PM::Matrix::Zero(1, globalPointCloud.getNbPoints());
	for(int i = 0; i < globalPointCloud.getNbPoints(); ++i)
	{
		int row = toGridCoordinate(globalPointCloud.features(0, i));
		int column = toGridCoordinate(globalPointCloud.features(1, i));
		int aisle = 0;
		if(is3D)
		{
			aisle = toGridCoordinate(globalPointCloud.features(2, i));
		}
		depths(0, i) = cellDepths[std::make_tuple(row, column, aisle)];
	}
	globalPointCloud.addDescriptor("depths", depths);

	cellManagerLock.lock();
	std::unordered_set<CellInfo> savedCellInfos = cellManager->getAllCellInfos();
	cellManagerLock.unlock();
	for(const auto& savedCellInfo: savedCellInfos)
	{
		if(currentLoadedCellInfos.find(savedCellInfo) == currentLoadedCellInfos.end())
		{
			cellManagerLock.lock();
			std::pair<CellInfo, PM::DataPoints> cell = cellManager->retrieveCell(savedCellInfo.row, savedCellInfo.column, savedCellInfo.aisle, savedCellInfo.depth);
			cellManagerLock.unlock();
			cell.second.addDescriptor("depths", PM::Matrix::Constant(1, cell.second.getNbPoints(), cell.first.depth));
			globalPointCloud.concatenate(cell.second);
		}
	}
	return globalPointCloud;
}

void norlab_icp_mapper::Map::setGlobalPointCloud(const PM::DataPoints& newLocalPointCloud)
{
	if(computeProbDynamic && !newLocalPointCloud.descriptorExists("normals"))
	{
		throw std::runtime_error("compute prob dynamic is set to true, but field normals does not exist for map points.");
	}

	localPointCloudLock.lock();
	localPointCloud = newLocalPointCloud;

	icpMapLock.lock();
	icp.setMap(localPointCloud);
	icpMapLock.unlock();

	localPointCloudEmpty.store(localPointCloud.getNbPoints() == 0);

	firstPoseUpdate.store(true);
	localPointCloudLock.unlock();
}

bool norlab_icp_mapper::Map::isLocalPointCloudEmpty() const
{
	return localPointCloudEmpty.load();
}
