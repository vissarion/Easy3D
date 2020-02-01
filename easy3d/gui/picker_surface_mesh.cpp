/**
 * Copyright (C) 2015 by Liangliang Nan (liangliang.nan@gmail.com)
 * https://3d.bk.tudelft.nl/liangliang/
 *
 * This file is part of Easy3D. If it is useful in your research/work,
 * I would be grateful if you show your appreciation by citing it:
 * ------------------------------------------------------------------
 *      Liangliang Nan.
 *      Easy3D: a lightweight, easy-to-use, and efficient C++
 *      library for processing and rendering 3D data. 2018.
 * ------------------------------------------------------------------
 * Easy3D is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License Version 3
 * as published by the Free Software Foundation.
 *
 * Easy3D is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */


#include <easy3d/gui/picker_surface_mesh.h>
#include <easy3d/viewer/shader_program.h>
#include <easy3d/viewer/shader_manager.h>
#include <easy3d/viewer/framebuffer_object.h>
#include <easy3d/viewer/renderer.h>
#include <easy3d/viewer/opengl_error.h>
#include <easy3d/viewer/drawable_triangles.h>
#include <easy3d/util/logging.h>


namespace easy3d {

    SurfaceMeshPicker::SurfaceMeshPicker(Camera *cam)
            : Picker(cam), hit_resolution_(15), program_(nullptr) {
        use_gpu_ = true;
    }


    SurfaceMeshPicker::~SurfaceMeshPicker() {
    }


    SurfaceMesh::Face SurfaceMeshPicker::pick_face(SurfaceMesh *model, int x, int y) {
        if (!model)
            return SurfaceMesh::Face();

        if (use_gpu_) {
            program_ = ShaderManager::get_program("selection/selection_single_primitive");
            if (!program_) {
                std::vector<ShaderProgram::Attribute> attributes;
                attributes.push_back(ShaderProgram::Attribute(ShaderProgram::POSITION, "vtx_position"));
                program_ = ShaderManager::create_program_from_files("selection/selection_single_primitive", attributes);
            }
            if (!program_)
                use_gpu_ = false;
        }

        if (use_gpu_)
            return pick_face_gpu(model, x, y);
        else // CPU with OpenMP (if supported)
            return pick_face_cpu(model, x, y);
    }


    SurfaceMesh::Vertex
    SurfaceMeshPicker::pick_vertex(SurfaceMesh *model, SurfaceMesh::Face picked_face, int x, int y) {
        if (!picked_face.is_valid() || picked_face != picked_face_) {
            LOG(ERROR) << "user provided face is not valid";
            return SurfaceMesh::Vertex();
        }

        double squared_distance = FLT_MAX;
        SurfaceMesh::Vertex closest_vertex;
        for (auto h : model->halfedges(picked_face)) {
            SurfaceMesh::Vertex v = model->to_vertex(h);
            double s = distance2(model->position(v), picked_point_);
            if (s < squared_distance) {
                squared_distance = s;
                closest_vertex = v;
            }
        }

        //////////////////////////////////////////////////////////////////////////

        // always check
        if (!closest_vertex.is_valid())
            return closest_vertex;

        const vec3 &p = model->position(closest_vertex);
        const vec2 &q = project(p);
        float dist = distance(q, vec2(static_cast<float>(x), static_cast<float>(y)));
        if (dist < hit_resolution_)
            return closest_vertex;
        else
            return SurfaceMesh::Vertex();
    }


    SurfaceMesh::Vertex SurfaceMeshPicker::pick_vertex(SurfaceMesh *model, int x, int y) {
        SurfaceMesh::Face face = pick_face(model, x, y);
        return pick_vertex(model, face, x, y);
    }


    SurfaceMesh::Halfedge
    SurfaceMeshPicker::pick_edge(SurfaceMesh *model, SurfaceMesh::Face picked_face, int x, int y) {
        if (!picked_face.is_valid() || picked_face != picked_face_) {
            LOG(ERROR) << "user provided face is not valid";
            return SurfaceMesh::Halfedge();
        }

        double squared_distance = FLT_MAX;
        SurfaceMesh::Halfedge closest_edge;

        // for edges that have duplicated vertices
        static double threshold = 1e-10;
        for (auto h : model->halfedges(picked_face)) {
            const vec3 &s = model->position(model->from_vertex(h));
            const vec3 &t = model->position(model->to_vertex(h));
            if (distance2(s, t) > threshold) {
                Segment3 seg(s, t);
                double d = seg.squared_ditance(picked_point_);
                if (d < squared_distance/* && seg.projected_inside(point)*/) {
                    squared_distance = d;
                    closest_edge = h;
                }
            }
        }

        //////////////////////////////////////////////////////////////////////////

        // always check
        if (!closest_edge.is_valid())
            return SurfaceMesh::Halfedge();

        const vec3 &s = model->position(model->from_vertex(closest_edge));
        const vec3 &t = model->position(model->to_vertex(closest_edge));
        const Segment2 seg(project(s), project(t));
        float s_dist = seg.squared_ditance(vec2(static_cast<float>(x), static_cast<float>(y)));
        float dist = std::sqrt(s_dist);

        if (dist < hit_resolution_)
            return closest_edge;
        else
            return SurfaceMesh::Halfedge();
    }


    SurfaceMesh::Halfedge SurfaceMeshPicker::pick_edge(SurfaceMesh *model, int x, int y) {
        SurfaceMesh::Face facet = pick_face(model, x, y);
        return pick_edge(model, facet, x, y);
    }


    Plane3 SurfaceMeshPicker::face_plane(SurfaceMesh *model, SurfaceMesh::Face face) const {
        auto h = model->halfedge(face);
        auto v = model->to_vertex(h);
        return Plane3(model->position(v), model->compute_face_normal(face));
    }


    SurfaceMesh::Face SurfaceMeshPicker::picked_face() const {
        if (!picked_face_.is_valid()) {
            LOG(ERROR) << "no face has been picked";
        }
        return picked_face_;
    }


    vec3 SurfaceMeshPicker::picked_point() const {
        if (!picked_face_.is_valid()) {
            LOG(ERROR) << "no face has been picked";
        }
        return picked_point_;
    }


    SurfaceMesh::Face SurfaceMeshPicker::pick_face_cpu(SurfaceMesh *model, int x, int y) {
        int num = model->faces_size();
        const vec3 &p_near = unproject(x, y, 0);
        const vec3 &p_far = unproject(x, y, 1);
        const OrientedLine3 oline(p_near, p_far);

        std::vector<char> status(num, 0);
#pragma omp parallel for
        for (int i = 0; i < num; ++i) {
            if (do_intersect(model, SurfaceMesh::Face(i), oline))
                status[i] = 1;
        }

        picked_face_ = SurfaceMesh::Face();
        double squared_distance = FLT_MAX;
        const Line3 &line = picking_line(x, y);
        for (int i = 0; i < num; ++i) {
            if (status[i]) {
                const SurfaceMesh::Face face(i);
                const Plane3 plane = face_plane(model, face);

                vec3 p;
                if (plane.intersect(line, p)) {
                    double s = distance2(p, p_near);
                    if (s < squared_distance) {
                        squared_distance = s;
                        picked_face_ = face;
                        picked_point_ = p;
                    }
                } else {
                    // If reached here, a parallel facet with distance less than hit resolution should be
                    // the candidate. However, the picking line does not intersect the facet.
                    // Logger::err(title()) << "should have intersection" << std::endl;
                }
            }
        }

        return picked_face_;
    }


    SurfaceMesh::Face SurfaceMeshPicker::pick_face_gpu(SurfaceMesh *model, int x, int y) {
        auto drawable = model->triangles_drawable("faces");
        if (!drawable) {
            drawable = model->add_triangles_drawable("faces");
            renderer::update_data(model, drawable);
        }

        int viewport[4];
        glGetIntegerv(GL_VIEWPORT, viewport);
        int width = viewport[2];
        int height = viewport[3];

        // prepare a frame buffer object (fbo), I will do offscreen rendering to the new fbo
        if (!fbo_) {
            fbo_ = new FramebufferObject(width, height, 0);
            fbo_->add_color_buffer();
            fbo_->add_depth_buffer();
        }
        fbo_->ensure_size(width, height);

        //--------------------------------------------------------------------------
        // render the 'scene' to the new FBO.

        // TODO: the performance can be improved. Since the 'scene' is static, we need to render it to the fbo only
        //       once. Then just query. Re-rendering is needed only when the scene is changed/manipulated, or the canvas
        //       size is changed.

        // Bind the offscreen fbo for drawing
        fbo_->bind();

        float color[4];
        glGetFloatv(GL_COLOR_CLEAR_VALUE, color);
        glClearColor(1.0f, 1.0f, 1.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        easy3d_debug_gl_error;
        easy3d_debug_frame_buffer_error;

        program_->bind();
        program_->set_uniform("MVP", camera()->modelViewProjectionMatrix());
        drawable->gl_draw(false);
        program_->release();

        // --- Maybe this is not necessary ---------
        glFlush();
        glFinish();
        // -----------------------------------------

        int gl_x, gl_y;
        screen_to_opengl(x, y, gl_x, gl_y, width, height);

        unsigned char c[4];
        fbo_->read_color(c, gl_x, gl_y);

        // switch back to the previous fbo
        fbo_->release();
        easy3d_debug_gl_error;
        easy3d_debug_frame_buffer_error;

        // restore the clear color
        glClearColor(color[0], color[1], color[2], color[3]);

        //--------------------------------------------------------------------------

        // Convert the color back to an integer ID
        int id = rgb::rgba(c[0], c[1], c[2], c[3]);
        picked_face_ = SurfaceMesh::Face();

#if 0 // If the mesh is a triangle mesh.
        picked_face_ = SurfaceMesh::Face(id);
        return picked_face_;
#else
        if (id >= 0) {
            // We draw the polygonal faces as triangles and the picked id is the index of the picked triangle. So we
            // need to figure out from which face this triangle comes from.
            auto triangle_range = model->get_face_property<std::pair<int, int> >("f:triangle_range");
            if (!triangle_range) {
                LOG(ERROR) << "face property 'f:triangle_range' not defined. Selection aborted";
                return SurfaceMesh::Face();
            }

            // Triangle meshes are more common... So treat is as a triangle mesh and check if the id is with the range.
            if (static_cast<unsigned int>(id) < model->faces_size()) {
                auto face = SurfaceMesh::Face(id);
                const auto &range = triangle_range[SurfaceMesh::Face(face)];
                if (id >= range.first && id <= range.second) {
                    picked_face_ = face;
                    return picked_face_;
                }
            }

            // Now treat the model as a general polygonal mesh.
            for (unsigned int face_id = 0; face_id < model->faces_size(); ++face_id) {
                const auto &range = triangle_range[SurfaceMesh::Face(face_id)];
                if (id >= range.first && id <= range.second) {
                    picked_face_ = SurfaceMesh::Face(face_id);
                    return picked_face_;
                }
            }
        }
#endif

        return SurfaceMesh::Face();
    }

}