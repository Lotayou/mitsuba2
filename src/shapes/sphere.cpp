#include <mitsuba/core/fwd.h>
#include <mitsuba/core/math.h>
#include <mitsuba/core/properties.h>
#include <mitsuba/core/transform.h>
#include <mitsuba/core/util.h>
#include <mitsuba/core/warp.h>
#include <mitsuba/render/bsdf.h>
#include <mitsuba/render/emitter.h>
#include <mitsuba/render/fwd.h>
#include <mitsuba/render/interaction.h>
#include <mitsuba/render/sensor.h>
#include <mitsuba/render/shape.h>

NAMESPACE_BEGIN(mitsuba)

/**
 * Sphere shape.
 */
template <typename Float, typename Spectrum>
class Sphere final : public Shape<Float, Spectrum> {
public:
    MTS_DECLARE_PLUGIN(Sphere, Shape)
    MTS_USING_BASE(Shape, bsdf, emitter, is_emitter);
    using Index = typename Base::Index;
    using Size  = typename Base::Size;

    Sphere(const Properties &props) : Base(props) {
        m_object_to_world =
            Transform4f::translate(Vector3f(props.point3f("center", Point3f(0.f))));
        m_radius = props.float_("radius", 1.f);

        if (props.has_property("to_world")) {
            Transform4f object_to_world = props.transform("to_world");
            Float radius = norm(object_to_world * Vector3f(1, 0, 0));
            // Remove the scale from the object-to-world transform
            m_object_to_world =
                object_to_world
                * Transform4f::scale(Vector3f(1.f / radius))
                * m_object_to_world;
            m_radius *= radius;
        }

        /// Are the sphere normals pointing inwards? default: no
        m_flip_normals = props.bool_("flip_normals", false);
        m_center = m_object_to_world * Point3f(0, 0, 0);
        m_world_to_object = m_object_to_world.inverse();
        m_inv_surface_area = 1.f / (4.f * math::Pi<Float> * m_radius * m_radius);

        if (m_radius <= 0.f) {
            m_radius = std::abs(m_radius);
            m_flip_normals = !m_flip_normals;
        }

        if (is_emitter())
            emitter()->set_shape(this);
    }

    BoundingBox3f bbox() const override {
        BoundingBox3f bbox;
        bbox.min = m_center - m_radius;
        bbox.max = m_center + m_radius;
        return bbox;
    }

    Float surface_area() const override {
        return 4.f * math::Pi<Float> * m_radius * m_radius;
    }

    // =============================================================
    //! @{ \name Sampling routines
    // =============================================================

    MTS_INLINE PositionSample3f sample_position(Float time, const Point2f &sample,
                                                Mask /*active*/) const override {
        Point3f p = warp::square_to_uniform_sphere(sample);

        PositionSample3f ps;
        ps.p = fmadd(p, m_radius, m_center);
        ps.n = p;

        if (m_flip_normals)
            ps.n = -ps.n;

        ps.time = time;
        ps.delta = m_radius != 0.f;
        ps.pdf = m_inv_surface_area;

        return ps;
    }

    MTS_INLINE Float pdf_position(const PositionSample3f & /*ps*/, Mask /*active*/) const override {
        return m_inv_surface_area;
    }

    MTS_INLINE DirectionSample3f sample_direction(const Interaction3f &it, const Point2f &sample,
                                                 Mask active) const override {
        DirectionSample3f result = zero<DirectionSample3f>();

        Vector3f dc_v = m_center - it.p;
        Float dc_2 = squared_norm(dc_v);

        Float radius_adj = m_radius * (m_flip_normals ? (1.f + math::Epsilon<Float>) :
                                                        (1.f - math::Epsilon<Float>));
        Mask outside_mask = active && dc_2 > sqr(radius_adj);
        if (likely(any(outside_mask))) {
            Float inv_dc            = rsqrt(dc_2),
                  sin_theta_max     = m_radius * inv_dc,
                  sin_theta_max_2   = sqr(sin_theta_max),
                  inv_sin_theta_max = rcp(sin_theta_max),
                  cos_theta_max     = safe_sqrt(1.f - sin_theta_max_2);

            /* Fall back to a Taylor series expansion for small angles, where
               the standard approach suffers from severe cancellation errors */
            Float sin_theta_2 = select(sin_theta_max_2 > 0.00068523f, /* sin^2(1.5 deg) */
                                       1.f - sqr(fmadd(cos_theta_max - 1.f, sample.x(), 1.f)),
                                       sin_theta_max_2 * sample.x()),
                  cos_theta = safe_sqrt(1.f - sin_theta_2);

            // Based on https://www.akalin.com/sampling-visible-sphere
            Float cos_alpha = sin_theta_2 * inv_sin_theta_max +
                              cos_theta * safe_sqrt(fnmadd(sin_theta_2, sqr(inv_sin_theta_max), 1.f)),
                  sin_alpha = safe_sqrt(fnmadd(cos_alpha, cos_alpha, 1.f));

            auto [sin_phi, cos_phi] = sincos(sample.y() * (2.f * math::Pi<Float>));

            Vector3f d = Frame3f(dc_v * -inv_dc).to_world(Vector3f(
                cos_phi * sin_alpha,
                sin_phi * sin_alpha,
                cos_alpha));

            DirectionSample3f ds = zero<DirectionSample3f>();
            ds.p        = fmadd(d, m_radius, m_center);
            ds.n        = d;
            ds.d        = ds.p - it.p;

            Float dist2 = squared_norm(ds.d);
            ds.dist     = sqrt(dist2);
            ds.d        = ds.d / ds.dist;
            ds.pdf      = warp::square_to_uniform_cone_pdf(zero<Vector3f>(), cos_theta_max);

            result[outside_mask] = ds;
        }

        Mask inside_mask = andnot(active, outside_mask);
        if (unlikely(any(inside_mask))) {
            Vector3f d = warp::square_to_uniform_sphere(sample);
            DirectionSample3f ds = zero<DirectionSample3f>();
            ds.p        = fmadd(d, m_radius, m_center);
            ds.n        = d;
            ds.d        = ds.p - it.p;

            Float dist2 = squared_norm(ds.d);
            ds.dist     = sqrt(dist2);
            ds.d        = ds.d / ds.dist;
            ds.pdf      = m_inv_surface_area * dist2 / abs_dot(ds.d, ds.n);

            result[inside_mask] = ds;
        }

        result.time = it.time;
        result.delta = m_radius != 0;

        if (m_flip_normals)
            result.n = -result.n;

        return result;
    }

    MTS_INLINE Float pdf_direction(const Interaction3f &it, const DirectionSample3f &ds,
                                   Mask /*active*/) const override {
        // Sine of the angle of the cone containing the sphere as seen from 'it.p'.
        Float sin_alpha = m_radius * rcp(norm(m_center - it.p)),
              cos_alpha = enoki::safe_sqrt(1.f - sin_alpha * sin_alpha);

        return select(sin_alpha < (1.f - math::Epsilon<Float>),
            // Reference point lies outside the sphere
            warp::square_to_uniform_cone_pdf(zero<Vector3f>(), cos_alpha),
            m_inv_surface_area * sqr(ds.dist) / abs_dot(ds.d, ds.n)
        );
    }

    //! @}
    // =============================================================

    // =============================================================
    //! @{ \name Ray tracing routines
    // =============================================================

    MTS_INLINE std::pair<Mask, Float> ray_intersect(const Ray3f &ray, Float * /*cache*/,
                                                    Mask active) const override {
        using Float64  = float64_array_t<Float>;

        Float64 mint = Float64(ray.mint);
        Float64 maxt = Float64(ray.maxt);

        Vector3d o = Vector3d(ray.o) - Vector3d(m_center);
        Vector3d d(ray.d);

        Float64 A = squared_norm(d);
        Float64 B = 2.0 * dot(o, d);
        Float64 C = squared_norm(o) - sqr((double) m_radius);

        auto [solution_found, near_t, far_t] = math::solve_quadratic(A, B, C);

        // Sphere doesn't intersect with the segment on the ray
        Mask out_bounds = !(near_t <= maxt && far_t >= mint); // NaN-aware conditionals

        // Sphere fully contains the segment of the ray
        Mask in_bounds = near_t < mint && far_t > maxt;

        Mask valid_intersection =
            active && solution_found && !out_bounds && !in_bounds;

        return { valid_intersection, select(near_t < mint, far_t, near_t) };
    }

    MTS_INLINE Mask ray_test(const Ray3f &ray, Mask active) const override {
        using Float64 = float64_array_t<Float>;

        Float64 mint = Float64(ray.mint);
        Float64 maxt = Float64(ray.maxt);

        Vector3d o = Vector3d(ray.o) - Vector3d(m_center);
        Vector3d d(ray.d);

        Float64 A = squared_norm(d);
        Float64 B = 2.0 * dot(o, d);
        Float64 C = squared_norm(o) - sqr((double) m_radius);

        auto [solution_found, near_t, far_t] = math::solve_quadratic(A, B, C);

        // Sphere doesn't intersect with the segment on the ray
        Mask out_bounds = !(near_t <= maxt && far_t >= mint); // NaN-aware conditionals

        // Sphere fully contains the segment of the ray
        Mask in_bounds  = near_t < mint && far_t > maxt;

        return solution_found && !out_bounds && !in_bounds && active;
    }

    MTS_INLINE void fill_surface_interaction(const Ray3f &ray, const Float * /*cache*/,
                                             SurfaceInteraction3f &si_out,
                                             Mask active) const override {
        SurfaceInteraction3f si(si_out);

        if constexpr (is_diff_array_v<Float>) {
            // Recompute the intersection if derivative information is desired.
            Vector3f o = ray.o - m_center;
            Float A = squared_norm(ray.d);
            Float B = 2.f * dot(o, ray.d);
            Float C = squared_norm(o) - sqr(m_radius);

            auto [solution_found, near_t, far_t] = math::solve_quadratic(A, B, C);

            // Sphere doesn't intersect with the segment on the ray
            Mask out_bounds = !(near_t <= ray.maxt && far_t >= ray.mint); // NaN-aware conditionals

            // Sphere fully contains the segment of the ray
            Mask in_bounds = near_t < ray.mint && far_t > ray.maxt;

            Mask valid_intersection =
                active && solution_found && !out_bounds && !in_bounds;

            si.t[valid_intersection] = select(near_t < ray.mint, far_t, near_t);
        }

        si.sh_frame.n = normalize(ray(si.t) - m_center);

        // Re-project onto the sphere to improve accuracy
        si.p = fmadd(si.sh_frame.n, m_radius, m_center);

        Vector3f local   = m_world_to_object * (si.p - m_center),
                d       = local / m_radius;

        Float   rd_2    = sqr(d.x()) + sqr(d.y()),
                theta   = unit_angle_z(d),
                phi     = atan2(d.y(), d.x());

        masked(phi, phi < 0.f) += 2.f * math::Pi<Float>;

        si.uv = Point2f(phi * math::InvTwoPi<Float>, theta * math::InvPi<Float>);
        si.dp_du = Vector3f(-local.y(), local.x(), 0.f);

        Float rd      = sqrt(rd_2),
              inv_rd  = rcp(rd),
              cos_phi = d.x() * inv_rd,
              sin_phi = d.y() * inv_rd;

        si.dp_dv = Vector3f(local.z() * cos_phi,
                           local.z() * sin_phi,
                           -rd * m_radius);

        Mask singularity_mask = active && eq(rd, 0.f);
        if (unlikely(any(singularity_mask)))
            si.dp_dv[singularity_mask] = Vector3f(m_radius, 0.f, 0.f);

        si.dp_du = m_object_to_world * si.dp_du * (2.f * math::Pi<Float>);
        si.dp_dv = m_object_to_world * si.dp_dv * math::Pi<Float>;

        if (m_flip_normals)
            si.sh_frame.n = -si.sh_frame.n;

        si.n = si.sh_frame.n;
        si.time = ray.time;

        si_out[active] = si;
    }

    MTS_INLINE std::pair<Vector3f, Vector3f> normal_derivative(const SurfaceInteraction3f &si,
                                                               bool /*shading_frame*/,
                                                               Mask /*active*/) const override {
        Float inv_radius = (m_flip_normals ? -1.f : 1.f) / m_radius;
        return { si.dp_du * inv_radius, si.dp_dv * inv_radius };
    }

    //! @}
    // =============================================================

    Size primitive_count() const override { return 1; }

    Size effective_primitive_count() const override { return 1; }

    std::string to_string() const override {
        std::ostringstream oss;
        oss << "Sphere[" << std::endl
            << "  radius = "  << m_radius << "," << std::endl
            << "  center = "  << m_center << std::endl
            << "]";
        return oss.str();
    }

private:
    Transform4f m_object_to_world;
    Transform4f m_world_to_object;
    Point3f m_center;
    ScalarFloat   m_radius;
    ScalarFloat   m_inv_surface_area;
    bool    m_flip_normals;
};

MTS_IMPLEMENT_PLUGIN(Sphere, Shape, "Sphere intersection primitive");
NAMESPACE_END(mitsuba)
