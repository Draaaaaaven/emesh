#include "MeshFlow2D.h"
#include "generic/geometry/TriangulationRefinement.hpp"
#include "generic/tree/QuadTreeUtilityMT.hpp"
#include "generic/geometry/HashFunction.hpp"
#include "generic/geometry/Transform.hpp"
#include "generic/geometry/Utility.hpp"
#include "generic/tools/FileSystem.hpp"
#include "generic/tools/Tools.hpp"
#include "generic/tools/Log.hpp"
#include "MeshIO.h"
#include <boost/geometry/index/rtree.hpp>
#include <boost/polygon/voronoi_builder.hpp>
#include <boost/polygon/voronoi.hpp>
using namespace generic;
using namespace emesh;
bool MeshFlow2D::LoadGeometryFiles(const std::string & filename, FileFormat format, PolygonContainer & polygons)
{
    polygons.clear();
    try {
        switch (format) {
            case FileFormat::DomDmc : {
                std::string dom = filename + ".dom";
                std::string dmc = filename + ".dmc";
                auto results = std::make_unique<std::map<int, SPtr<PolygonContainer> > >();
                if(!io::ImportDomDmcFiles(dom, dmc, *results)) return false;
                for(const auto & result : *results){
                    if(result.second){
                        for(auto & polygon : *result.second)
                            polygons.emplace_back(std::move(polygon));
                    }
                }
                return true;
            }
            case FileFormat::WKT : {
                std::string wkt = filename + ".wkt";
                auto pwhs = std::make_unique<PolygonWithHolesContainer>();
                if(!io::ImportWktFile(wkt, *pwhs)) return false;
                for(auto & pwh : *pwhs){
                    polygons.emplace_back(std::move(pwh.outline));
                    for(auto & hole : pwh.holes)
                        polygons.emplace_back(std::move(hole));
                }
                return true;
            }
            default : return false;
        }
    }
    catch (...) { return false; }
    return true;
}

bool MeshFlow2D::ExtractIntersections(const PolygonContainer & polygons, Segment2DContainer & segments)
{
    segments.clear();
    for(const auto & polygon : polygons){
        size_t size = polygon.Size();
        for(size_t i = 0; i < size; ++i){
            size_t j = (i + 1) % size;
            segments.emplace_back(Segment2D<coor_t>(polygon[i], polygon[j]));
        }
    }
    std::vector<Segment2D<coor_t> > results;
    boost::polygon::intersect_segments(results, segments.begin(), segments.end());
    segments.clear();
    segments.insert(segments.end(), results.begin(), results.end());
    RemoveDuplicatedSegments(segments);
    return true;
}

bool MeshFlow2D::RemoveDuplicatedSegments(Segment2DContainer & segments)
{
    using EdgeSet = topology::UndirectedIndexEdgeSet;
    using PointIdxMap = std::unordered_map<Point2D<coor_t>, size_t, PointHash<coor_t> >;
    
    EdgeSet edgeSet;
    size_t index = 0;
    PointIdxMap pointIdxMap;
    auto getIndex = [&pointIdxMap, &index](const Point2D<coor_t> & p) mutable
    {
        if(!pointIdxMap.count(p)){
            pointIdxMap.insert(std::make_pair(p, index++));
        }
        return pointIdxMap.at(p);
    };

    auto size = segments.size();
    auto iter = segments.begin();
    while(iter != segments.end()){
        IndexEdge e(getIndex((*iter)[0]), getIndex((*iter)[1]));
        if(edgeSet.count(e)){
            iter = segments.erase(iter);
        }
        else{
            edgeSet.emplace(std::move(e));
            ++iter;
        }
    }
    log::Debug("remove duplicated edges: %1%", size - segments.size());
    return true;   
}


bool MeshFlow2D::MergeClosestParallelSegmentsIteratively(Segment2DContainer & segments, coor_t tolerance)
{
    if(tolerance <= 0) return true;

    size_t times = 0;
    bool flag = true;
    while(flag){
        log::Info("merge parallel segments in iteration: %1%", ++times);
        flag = MergeClosestParallelSegments(segments, tolerance);
        RemoveDuplicatedSegments(segments);//wbtest
    }
    return true;
}

bool MeshFlow2D::ExtractTopology(const Segment2DContainer & segments, Point2DContainer & points, std::list<IndexEdge> & edges)
{
    points.clear();
    edges.clear();
    using EdgeSet = topology::UndirectedIndexEdgeSet;
    using PointIdxMap = std::unordered_map<Point2D<coor_t>, size_t, PointHash<coor_t> >;
    
    EdgeSet edgeSet;
    PointIdxMap pointIdxMap;

    auto getIndex = [&pointIdxMap, &points](const Point2D<coor_t> & p) mutable
    {
        if(!pointIdxMap.count(p)){
            pointIdxMap.insert(std::make_pair(p, points.size()));
            points.push_back(p);
        }
        return pointIdxMap.at(p);
    };

    points.reserve(2 * segments.size());
    for(const auto & segment : segments){
        IndexEdge e(getIndex(segment[0]), getIndex(segment[1]));
        if(edgeSet.count(e)) continue;
        edgeSet.insert(e);
        edges.emplace_back(std::move(e));
    }
    points.shrink_to_fit();
    return true;   
}


bool MeshFlow2D::MergeClosePointsAndRemapEdge(Point2DContainer & points, std::list<IndexEdge> & edges, coor_t tolerance)
{
    if(tolerance > 0)
        geometry::tri::RemoveDuplicatesAndRemapEdges(points, edges, tolerance);
    return true; 
}

bool MeshFlow2D::SplitOverlengthEdges(Point2DContainer & points, std::list<IndexEdge> & edges, coor_t maxLength)
{
    if(0 == maxLength) return true;
    coor_t maxLenSq = maxLength * maxLength;

    auto lenSq = [&points](const IndexEdge & e) { return DistanceSq(points[e.v1()], points[e.v2()]); };
    auto split = [&points](const IndexEdge & e) mutable
    {
        size_t index = points.size();
        points.push_back((points[e.v1()] + points[e.v2()]) * 0.5);
        return std::make_pair(IndexEdge(e.v1(), index), IndexEdge(index, e.v2()));
    };

    auto iter = edges.begin();
    while(iter != edges.end()){
        if(lenSq(*iter) <= maxLenSq){
            ++iter; continue;
        }
        else{
            auto [e1, e2] = split(*iter);
            iter = edges.erase(iter);
            iter = edges.insert(iter, e2);
            iter = edges.insert(iter, e1);
        }
    }
    return true;
}

bool MeshFlow2D::AddPointsFromBalancedQuadTree(Point2DContainer & points, std::list<IndexEdge> & edges, size_t maxLevel)
{
    Box2D<coor_t> bbox;
    std::list<Point2D<coor_t> * > pts;
    for(auto & point : points){
        bbox |= point;
        pts.push_back(&point);
    }

    using QuadTree = tree::QuadTree<coor_t, Point2D<coor_t>, PointExtent>;
    using QuadNode = typename QuadTree::QuadNode;
    using RectNode = std::pair<Box2D<coor_t>, CPtr<IndexEdge > >;
    using RectTree = boost::geometry::index::rtree<RectNode, boost::geometry::index::dynamic_rstar>;
    
    QuadTree quadTree(bbox);
    quadTree.Build(pts, 0);
    auto condition = [&maxLevel](QuadNode * node)
    {
        auto bbox = node->GetBBox();
        return node->GetLevel() < maxLevel && node->GetObjs().size() > 0 && std::min(bbox.Length(), bbox.Width()) > 1; 
    };

    QuadTree::CreateSubNodesIf(&quadTree, condition);

    quadTree.Balance();
    std::list<QuadNode * > leafNodes;
    QuadTree::GetAllLeafNodes(&quadTree, leafNodes);

    boost::geometry::index::dynamic_rstar para(16);
    RectTree rectTree(para);
    for(const auto & edge : edges){
        Box2D<coor_t> box;
        box |= points[edge.v1()];
        box |= points[edge.v2()];
        rectTree.insert(std::make_pair(box, &edge));
    }

    auto ptEdgeDistSq = [&points](const Point2D<coor_t> & p, const IndexEdge & edge)
    {
        return PointSegmentDistanceSq(p, Segment2D<coor_t>(points[edge.v1()], points[edge.v2()]));
    };

    std::vector<RectNode> rectNodes;
    for(auto node : leafNodes){
        if(node->GetObjs().size() > 0) continue;
        const auto & box = node->GetBBox();
        Point2D<coor_t> ct = box.Center().Cast<coor_t>();

        rectNodes.clear();
        Box2D<coor_t> searchBox(ct, ct);
        rectTree.query(boost::geometry::index::intersects(searchBox), std::back_inserter(rectNodes));

        if(rectNodes.empty()) points.emplace_back(std::move(ct));
        else{
            bool flag = true;
            float_t distSq = std::min(box.Width(), box.Length());
            distSq = 0.7 * distSq * distSq;
            for(const auto & rectNode : rectNodes){
                if(ptEdgeDistSq(ct, *rectNode.second) < distSq){
                    flag = false; break;
                }
            }
            if(flag) points.emplace_back(std::move(ct)); 
        }            
    }
    return true;
}

bool MeshFlow2D::TriangulatePointsAndEdges(const Point2DContainer & points, const std::list<IndexEdge> & edges, Triangulation<Point2D<coor_t> > & tri)
{
    tri.Clear();
    try {
        Triangulator2D<coor_t> triangulator(tri);
        triangulator.InsertVertices(points.begin(), points.end(), [](const Point2D<coor_t> & p){ return p[0]; }, [](const Point2D<coor_t> & p){ return p[1]; });
        triangulator.InsertEdges(edges.begin(), edges.end(), [](const IndexEdge & e){ return e.v1(); }, [](const IndexEdge & e){ return e.v2(); });
        triangulator.EraseOuterTriangles();
    }
    catch (...) {
        return false;
    }
    return true;
}

bool MeshFlow2D::TriangulationRefinement(Triangulation<Point2D<coor_t> > & triangulation, const Mesh2Ctrl & ctrl)
{
    // ChewSecondRefinement2D<coor_t> refinement(triangulation);
    // JonathanRefinement2D<coor_t> refinement(triangulation);
    // refinement.SetParas(ctrl.minAlpha, ctrl.minEdgeLen, ctrl.maxEdgeLen);
    // refinement.Refine(iteration);
    // refinement.ReallocateTriangulation();
    // return true;

    // Try iterations temporarily
    // Since the refinement algrithom is not very stable
    bool refined = false;
    size_t perStep = 5000;
    for(size_t i = 0; i < ctrl.refineIte; ++i){
        Triangulation<Point2D<coor_t> > backup = triangulation;
        try {
            JonathanRefinement2D<coor_t> refinement(backup);
            refinement.SetParas(ctrl.minAlpha, ctrl.minEdgeLen, ctrl.maxEdgeLen);
            refinement.Refine(perStep);
            refinement.ReallocateTriangulation();
            refined = refinement.Refined();  
        }
        catch (...){
            log::Info("failed to refine mesh at iteration %1%, will use former result", i + 1);
            return false;
        }
        std::swap(backup, triangulation);
        size_t nodes = triangulation.vertices.size();
        size_t elements = triangulation.triangles.size();
        log::Info("refine iteration: %1%/%2%, total nodes: %3%, total elements: %4%", i + 1, ctrl.refineIte, nodes, elements);
        
        if(refined){
            log::Info("mesh refined, stop iteration");
            break;
        }
    }
    return true;
}

bool MeshFlow2D::ExportMeshResult(const std::string & filename, FileFormat format, const Triangulation<Point2D<coor_t> > & triangulation)
{
    switch (format) {
        case FileFormat::MSH : {
            std::string msh = filename + ".msh";
            return io::ExportMshFile(msh, triangulation);
        }
        case FileFormat::VTK : {
            std::string vtk = filename + ".vtk";
            return io::ExportVtkFile(vtk, triangulation);
        }
        default : return false;
    }
}

bool MeshFlow2D::GenerateReport(const std::string & filename, const Triangulation<Point2D<coor_t> > & triangulation)
{
    TriangleEvaluator<Point2D<coor_t> > evaluator(triangulation);
    auto evaluation = evaluator.Report();
    auto rpt = filename + ".rpt";
    return io::ExportReportFile(filename, evaluation, false);
}

bool MeshFlow2D::MergeClosestParallelSegments(Segment2DContainer & segments, coor_t tolerance)
{
    if(tolerance <= 0) return true;

    using namespace boost::polygon;
    using Edge = voronoi_edge<float_t>;
    using Segment = typename Segment2DContainer::value_type;

    voronoi_diagram<float_t> vd;
    voronoi_builder<int32_t> builder;
    std::unordered_map<size_t, CPtr<Segment> > segIdxMap;

    log::Info("segment size: %1%", segments.size());
    for(auto & segment : segments){
        const auto & low  = segment[0];
        const auto & high = segment[1];
        auto index = builder.insert_segment(low[0], low[1], high[0], high[1]);
        segIdxMap.insert(std::make_pair(index, &segment));
    }
    builder.construct(&vd);

    std::list<Segment> added;
    std::unordered_set<CPtr<Edge> > visited;
    std::unordered_set<CPtr<Segment> > merged;
    
    float_t distSq = tolerance * tolerance;
    auto minDist = std::numeric_limits<float_t>::max();
    auto distFun = [&distSq](const Segment & s1, const Segment & s2)
    {
        auto d1 = PointSegmentDistanceSq(s1[0], s2);
        auto d2 = PointSegmentDistanceSq(s1[1], s2);
        if(math::LT(d1, distSq) && math::LT(d2, distSq)) return std::max(d1, d2);
        
        auto d3 = PointSegmentDistanceSq(s2[0], s1);
        auto d4 = PointSegmentDistanceSq(s2[1], s1);
        if(math::LT(d3, distSq) && math::LT(d4, distSq)) return std::max(d3, d4);

        return std::numeric_limits<float_t>::max();;
    };

    size_t idx1, idx2;
    for(auto iter = vd.edges().begin(); iter != vd.edges().end(); ++iter){
        if(!iter->is_primary()) continue;
        auto twin = iter->twin();
        auto self = twin->twin();
        if(visited.count(self)) continue;
        visited.insert(twin);

        auto cell = iter->cell();
        idx1 = cell->source_index();
        cell = twin->cell();
        idx2 = cell->source_index();
        CPtr<Segment> seg1 = segIdxMap.at(idx1);
        CPtr<Segment> seg2 = segIdxMap.at(idx2);
        if(merged.count(seg1) || merged.count(seg2))
            continue;

        // if(0 != orientation(*seg1, *seg2)) continue;
        auto currSq = distFun(*seg1, *seg2);
        if(math::GT(currSq, distSq)) continue;
        // if(math::EQ(currSq, float_t(0))) continue;
        if(currSq < minDist) minDist = currSq;
        
        merged.insert(seg1);
        merged.insert(seg2);
        MergeClosestParallelSegments(*seg1, *seg2, added);
    }

    bool flag = merged.size() > 0 ? true : false;
    log::Info("minimum segment distance: %1%", std::sqrt(minDist));
    log::Info("merged near segments: %1%", merged.size());

    //remove merged segs
    if(flag){
        auto iter = segments.begin();
        while(iter != segments.end()){
            CPtr<Segment> curr = &(*iter);
            if(merged.count(curr)){
                merged.erase(curr);
                iter = segments.erase(iter);
            }
            else iter++;
        }
        //insert added segs
        segments.insert(segments.end(), added.begin(), added.end());
    }
    return flag;
}

void MeshFlow2D::MergeClosestParallelSegments(const Segment2D<coor_t> & seg1, const Segment2D<coor_t> & seg2, Segment2DContainer & added)
{
    std::vector<Point2D<coor_t> > points{ seg1[0], seg1[1], seg2[0], seg2[1] };
    auto minX = std::min({seg1[0][0], seg1[1][0], seg2[0][0], seg2[1][0]});
    auto maxX = std::max({seg1[0][0], seg1[1][0], seg2[0][0], seg2[1][0]});
    auto minY = std::min({seg1[0][1], seg1[1][1], seg2[0][1], seg2[1][1]});
    auto maxY = std::max({seg1[0][1], seg1[1][1], seg2[0][1], seg2[1][1]});
    size_t coord = (maxX - minX) > (maxY - minY) ? 0 : 1;
    auto func = [coord](const Point2D<coor_t> & p1, const Point2D<coor_t> & p2){ return p1[coord] < p2[coord]; };
    std::sort(points.begin(), points.end(), func);
    auto curr = points.begin();
    while(curr != points.end()){
        auto next = curr; next++;
        if(next != points.end() && (*curr)[coord] == (*next)[coord]){
            curr = points.erase(curr);
        }
        else curr++;
    }
    for(size_t i = 0; i < points.size() - 1; ++i){
        size_t j = i + 1;
        added.emplace_back(Segment2D<coor_t>(points[i], points[j]));
    }
}
