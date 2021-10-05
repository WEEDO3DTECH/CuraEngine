//Copyright (c) 2021 Ultimaker B.V.
//CuraEngine is released under the terms of the AGPLv3 or higher.

#include "LightningLayer.h" //The class we're implementing.

#include <iterator> // advance

#include "LightningDistanceField.h"
#include "LightningTreeNode.h"
#include "../sliceDataStorage.h"
#include "../utils/linearAlg2D.h"
#include "../utils/SVG.h"
#include "../utils/SparsePointGridInclusive.h"

using namespace cura;

coord_t LightningLayer::getWeightedDistance(const Point& boundary_loc, const Point& unsupported_location)
{
    return vSize(boundary_loc - unsupported_location);
}

Point GroundingLocation::p() const
{
    if (tree_node != nullptr)
    {
        return tree_node->getLocation();
    }
    else
    {
        assert(boundary_location);
        return boundary_location->p();
    }
}

void LightningLayer::fillLocator(SparseLightningTreeNodeGrid& tree_node_locator)
{
    std::function<void(LightningTreeNodeSPtr)> add_node_to_locator_func =
        [&tree_node_locator](LightningTreeNodeSPtr node)
        {
            tree_node_locator.insert(node->getLocation(), node);
        };
    for (auto& tree : tree_roots)
    {
        tree->visitNodes(add_node_to_locator_func);
    }
}

void LightningLayer::generateNewTrees(const Polygons& current_overhang, Polygons& current_outlines, coord_t supporting_radius)
{
    LightningDistanceField distance_field(supporting_radius, current_outlines, current_overhang);

    constexpr coord_t locator_cell_size = 2000;
    SparseLightningTreeNodeGrid tree_node_locator(locator_cell_size);
    fillLocator(tree_node_locator);

    // Until no more points need to be added to support all:
    // Determine next point from tree/outline areas via distance-field
    Point unsupported_location;
    while (distance_field.tryGetNextPoint(&unsupported_location))
    {
        constexpr coord_t min_dist_from_boundary_for_tree = 10;
        GroundingLocation grounding_loc = getBestGroundingLocation(unsupported_location, current_outlines, supporting_radius, min_dist_from_boundary_for_tree, tree_node_locator);

        LightningTreeNodeSPtr new_parent;
        LightningTreeNodeSPtr new_child;
        attach(unsupported_location, grounding_loc, new_child, new_parent);
        tree_node_locator.insert(new_child->getLocation(), new_child);
        if (new_parent)
        {
            tree_node_locator.insert(new_parent->getLocation(), new_parent);
        }

        // update distance field
        distance_field.update(grounding_loc.p(), unsupported_location);
    }
}

GroundingLocation LightningLayer::getBestGroundingLocation
(
    const Point& unsupported_location,
    const Polygons& current_outlines,
    const coord_t supporting_radius,
    const coord_t min_dist_from_boundary_for_tree,
    const SparseLightningTreeNodeGrid& tree_node_locator,
    const LightningTreeNodeSPtr& exclude_tree
)
{
    ClosestPolygonPoint cpp = PolygonUtils::findClosest(unsupported_location, current_outlines);
    Point node_location = cpp.p();

    LightningTreeNodeSPtr sub_tree{ nullptr };
    coord_t current_dist = getWeightedDistance(node_location, unsupported_location);
    if (current_dist >= min_dist_from_boundary_for_tree) // don't reconnect tree roots to other trees if they are already at/near the boundary
    {
        auto candidate_trees = tree_node_locator.getNearbyVals(unsupported_location, std::min(current_dist, supporting_radius));
        for (auto& candidate_wptr : candidate_trees)
        {
            auto candidate_sub_tree = candidate_wptr.lock();
            if (candidate_sub_tree && candidate_sub_tree != exclude_tree && !(exclude_tree && exclude_tree->hasOffspring(candidate_sub_tree)))
            {
                const coord_t candidate_dist = candidate_sub_tree->getWeightedDistance(unsupported_location, supporting_radius);
                if (candidate_dist < current_dist)
                {
                    current_dist = candidate_dist;
                    sub_tree = candidate_sub_tree;
                }
            }
        }
    }

    if (! sub_tree)
    {
        return GroundingLocation{ nullptr, cpp };
    }
    else
    {
        return GroundingLocation{ sub_tree, std::optional<ClosestPolygonPoint>() };
    }
}

bool LightningLayer::attach
(
    const Point& unsupported_location,
    const GroundingLocation& grounding_loc,
    LightningTreeNodeSPtr& new_child,
    LightningTreeNodeSPtr& new_root
)
{
    // Update trees & distance fields.
    if (grounding_loc.boundary_location)
    {
        new_root = LightningTreeNode::create(grounding_loc.p());
        new_child = new_root->addChild(unsupported_location);
        tree_roots.push_back(new_root);
        return true;
    }
    else
    {
        new_child = grounding_loc.tree_node->addChild(unsupported_location);
        return false;
    }
}

Point rootPolygonIntersection(const Point& inside_poly, const Point& old_root, const Polygons& current_outlines)
{
    Point result{ inside_poly };  // Not expected to stay that way if 'inside_poly' is indeed inside (defensive programming).
    size_t closest_dist2 = std::numeric_limits<coord_t>::max();

    for (const auto& poly : current_outlines)
    {
        const size_t poly_size = poly.size();
        for (size_t i_segment_start = 0; i_segment_start < poly_size; ++i_segment_start)
        {
            const size_t i_segment_end = (i_segment_start + 1) % poly_size;
            const Point& p_start = poly[i_segment_start];
            const Point& p_end = poly[i_segment_end];

            Point coll;
            if
            (
                LinearAlg2D::lineLineIntersection(inside_poly, old_root, p_start, p_end, coll) &&
                ! LinearAlg2D::pointIsProjectedBeyondLine(coll, p_start, p_end)
            )
            {
                const size_t dist2 = vSize2(old_root - coll);
                if (dist2 < closest_dist2)
                {
                    closest_dist2 = dist2;
                    result = coll;
                }
            }
        }
    }

    return result;
}

void LightningLayer::reconnectRoots(std::vector<LightningTreeNodeSPtr>& to_be_reconnected_tree_roots, const Polygons& current_outlines, const coord_t supporting_radius, const coord_t wall_supporting_radius)
{
    constexpr coord_t locator_cell_size = 2000;
    constexpr coord_t tree_connecting_ignore_offset = 100;

    SparseLightningTreeNodeGrid tree_node_locator(locator_cell_size);
    fillLocator(tree_node_locator);

    for (auto root_ptr : to_be_reconnected_tree_roots)
    {
        auto old_root_it = std::find(tree_roots.begin(), tree_roots.end(), root_ptr);

        coord_t tree_connecting_ignore_width = wall_supporting_radius - tree_connecting_ignore_offset; // Ideally, the boundary size in which the valence rule is ignored would be configurable.
        GroundingLocation ground = getBestGroundingLocation(root_ptr->getLocation(), current_outlines, supporting_radius, tree_connecting_ignore_width, tree_node_locator, root_ptr);
        if (ground.boundary_location)
        {
            if (ground.boundary_location.value().p() == root_ptr->getLocation())
            {
                continue; // Already on the boundary.
            }

            auto new_root = LightningTreeNode::create
                (
                    root_ptr->getLastGroundingLocation() ?
                        rootPolygonIntersection(root_ptr->getLocation(), root_ptr->getLastGroundingLocation().value(), current_outlines) :
                        ground.p()
                );
            new_root->addChild(root_ptr);
            tree_node_locator.insert(new_root->getLocation(), new_root);

            *old_root_it = std::move(new_root); // replace old root with new root
        }
        else
        {
            assert(ground.tree_node);
            assert(ground.tree_node != root_ptr);
            assert(!root_ptr->hasOffspring(ground.tree_node));
            assert(!ground.tree_node->hasOffspring(root_ptr));

            ground.tree_node->addChild(root_ptr);

            // remove old root
            *old_root_it = std::move(tree_roots.back());
            tree_roots.pop_back();
        }
    }
}

// Returns 'added someting'.
Polygons LightningLayer::convertToLines(const coord_t line_width) const
{
    Polygons result_lines;
    if (tree_roots.empty())
    {
        return result_lines;
    }

    for (const auto& tree : tree_roots)
    {
        tree->convertToPolylines(result_lines, line_width);
    }

    // TODO: allow for polylines!
    Polygons split_lines;
    for (PolygonRef line : result_lines)
    {
        if (line.size() <= 1) continue;
        Point last = line[0];
        for (size_t point_idx = 1; point_idx < line.size(); point_idx++)
        {
            Point here = line[point_idx];
            split_lines.addLine(last, here);
            last = here;
        }
    }

    return split_lines;
}