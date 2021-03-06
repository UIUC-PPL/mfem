// Copyright (c) 2019, Lawrence Livermore National Security, LLC. Produced at
// the Lawrence Livermore National Laboratory. LLNL-CODE-443211. All Rights
// reserved. See file COPYRIGHT for details.
//
// This file is part of the MFEM library. For more information and source code
// availability see http://mfem.org.
//
// MFEM is free software; you can redistribute it and/or modify it under the
// terms of the GNU Lesser General Public License (as published by the Free
// Software Foundation) version 2.1 dated February 1999.

#include "catch.hpp"
#include "mfem.hpp"
#include <fstream>
#include <iostream>

using namespace mfem;

namespace pa_kernels
{

double zero_field(const Vector &x)
{
   return 0.0;
}

void solenoidal_field2d(const Vector &x, Vector &u)
{
   u(0) = x(1);
   u(1) = -x(0);
}

void non_solenoidal_field2d(const Vector &x, Vector &u)
{
   u(0) = x(0) * x(1);
   u(1) = -x(0) + x(1);
}

double div_non_solenoidal_field2d(const Vector &x)
{
   return 1.0 + x(1);
}

void solenoidal_field3d(const Vector &x, Vector &u)
{
   double xi = x(0);
   double yi = x(1);
   double zi = x(2);

   u(0) = -cos(zi) * sin(xi);
   u(1) = -cos(xi) * cos(zi);
   u(2) = cos(xi) * sin(yi) + cos(xi) * sin(zi);
}

void non_solenoidal_field3d(const Vector &x, Vector &u)
{
   double xi = x(0);
   double yi = x(1);
   double zi = x(2);

   u(0) = cos(xi) * cos(yi);
   u(1) = sin(xi) * sin(zi);
   u(2) = cos(zi) * sin(xi);
}

double div_non_solenoidal_field3d(const Vector &x)
{
   double xi = x(0);
   double yi = x(1);
   double zi = x(2);
   return -cos(yi) * sin(xi) - sin(xi) * sin(zi);
}

double pa_divergence_testnd(int dim,
                            void (*f1)(const Vector &, Vector &),
                            double (*divf1)(const Vector &))
{
   Mesh *mesh = nullptr;
   if (dim == 2)
   {
      mesh = new Mesh(2, 2, Element::QUADRILATERAL, 0, 1.0, 1.0);
   }
   if (dim == 3)
   {
      mesh = new Mesh(2, 2, 2, Element::HEXAHEDRON, 0, 1.0, 1.0, 1.0);
   }

   int order = 4;

   // Vector valued
   H1_FECollection fec1(order, dim);
   FiniteElementSpace fes1(mesh, &fec1, dim);

   // Scalar
   H1_FECollection fec2(order, dim);
   FiniteElementSpace fes2(mesh, &fec2);

   GridFunction field(&fes1), field2(&fes2);

   MixedBilinearForm dform(&fes1, &fes2);
   dform.SetAssemblyLevel(AssemblyLevel::PARTIAL);
   dform.AddDomainIntegrator(new VectorDivergenceIntegrator);
   dform.Assemble();

   // Project u = f1
   VectorFunctionCoefficient fcoeff1(dim, f1);
   field.ProjectCoefficient(fcoeff1);

   // Check if div(u) = divf1
   dform.Mult(field, field2);
   FunctionCoefficient fcoeff2(divf1);
   LinearForm lf(&fes2);
   lf.AddDomainIntegrator(new DomainLFIntegrator(fcoeff2));
   lf.Assemble();
   field2 -= lf;

   delete mesh;

   return field2.Norml2();
}

TEST_CASE("PA VectorDivergence", "[PartialAssembly]")
{
   SECTION("2D")
   {
      // Check if div([y, -x]) == 0
      REQUIRE(pa_divergence_testnd(2, solenoidal_field2d, zero_field)
              == Approx(0.0));

      // Check if div([x*y, -x+y]) == 1 + y
      REQUIRE(pa_divergence_testnd(2,
                                   non_solenoidal_field2d,
                                   div_non_solenoidal_field2d)
              == Approx(0.0));
   }

   SECTION("3D")
   {
      // Check if
      // div([-Cos[z] Sin[x],
      //      -Cos[x] Cos[z],
      //       Cos[x] Sin[y] + Cos[x] Sin[z]) == 0
      REQUIRE(pa_divergence_testnd(3, solenoidal_field3d, zero_field)
              == Approx(0.0));

      // Check if
      // div([Cos[x] Cos[y],
      //      Sin[x] Sin[z],
      //      Cos[z] Sin[x]]) == -Cos[y] Sin[x] - Sin[x] Sin[z]
      REQUIRE(pa_divergence_testnd(3,
                                   non_solenoidal_field3d,
                                   div_non_solenoidal_field3d)
              == Approx(0.0));
   }
}

double testfunc(const Vector &x)
{
   double r = cos(x(0)) + sin(x(1));
   if (x.Size() == 3)
   {
      r += cos(x(2));
   }
   return r;
}

void grad_testfunc(const Vector &x, Vector &u)
{
   u(0) = -sin(x(0));
   u(1) = cos(x(1));
   if (x.Size() == 3)
   {
      u(2) = -sin(x(2));
   }
}

double pa_gradient_testnd(int dim,
                          double (*f1)(const Vector &),
                          void (*gradf1)(const Vector &, Vector &))
{
   Mesh *mesh = nullptr;
   if (dim == 2)
   {
      mesh = new Mesh(2, 2, Element::QUADRILATERAL, 0, 1.0, 1.0);
   }
   if (dim == 3)
   {
      mesh = new Mesh(2, 2, 2, Element::HEXAHEDRON, 0, 1.0, 1.0, 1.0);
   }

   int order = 4;

   // Scalar
   H1_FECollection fec1(order, dim);
   FiniteElementSpace fes1(mesh, &fec1);

   // Vector valued
   H1_FECollection fec2(order, dim);
   FiniteElementSpace fes2(mesh, &fec2, dim);

   GridFunction field(&fes1), field2(&fes2);

   MixedBilinearForm gform(&fes1, &fes2);
   gform.SetAssemblyLevel(AssemblyLevel::PARTIAL);
   gform.AddDomainIntegrator(new GradientIntegrator);
   gform.Assemble();

   // Project u = f1
   FunctionCoefficient fcoeff1(f1);
   field.ProjectCoefficient(fcoeff1);

   // Check if grad(u) = gradf1
   gform.Mult(field, field2);
   VectorFunctionCoefficient fcoeff2(dim, gradf1);
   LinearForm lf(&fes2);
   lf.AddDomainIntegrator(new VectorDomainLFIntegrator(fcoeff2));
   lf.Assemble();
   field2 -= lf;

   delete mesh;

   return field2.Norml2();
}

TEST_CASE("PA Gradient", "[PartialAssembly]")
{
   SECTION("2D")
   {
      // Check if grad(Cos[x] + Sin[y]) == [-Sin[x], Cos[y]]
      REQUIRE(pa_gradient_testnd(2, testfunc, grad_testfunc) == Approx(0.0));
   }

   SECTION("3D")
   {
      // Check if grad(Cos[x] + Sin[y] + Cos[z]) == [-Sin[x], Cos[y], -Sin[z]]
      REQUIRE(pa_gradient_testnd(3, testfunc, grad_testfunc) == Approx(0.0));
   }
}

double test_nl_convection_nd(int dim)
{
   Mesh *mesh;

   if (dim == 2)
   {
      mesh = new Mesh(2, 2, Element::QUADRILATERAL, 0, 1.0, 1.0);
   }
   if (dim == 3)
   {
      mesh = new Mesh(2, 2, 2, Element::HEXAHEDRON, 0, 1.0, 1.0, 1.0);
   }

   int order = 2;
   H1_FECollection fec(order, dim);
   FiniteElementSpace fes(mesh, &fec, dim);

   GridFunction x(&fes), y_fa(&fes), y_pa(&fes);
   x.Randomize(3);

   NonlinearForm nlf_fa(&fes);
   nlf_fa.AddDomainIntegrator(new VectorConvectionNLFIntegrator);
   nlf_fa.Mult(x, y_fa);

   NonlinearForm nlf_pa(&fes);
   nlf_pa.SetAssemblyLevel(AssemblyLevel::PARTIAL);
   nlf_pa.AddDomainIntegrator(new VectorConvectionNLFIntegrator);
   nlf_pa.Setup();
   nlf_pa.Mult(x, y_pa);

   y_fa -= y_pa;
   double difference = y_fa.Norml2();

   delete mesh;

   return difference;
}

TEST_CASE("Nonlinear Convection", "[PartialAssembly], [NonlinearPA]")
{
   SECTION("2D")
   {
      REQUIRE(test_nl_convection_nd(2) == Approx(0.0));
   }

   SECTION("3D")
   {
      REQUIRE(test_nl_convection_nd(3) == Approx(0.0));
   }
}

template <typename INTEGRATOR>
double test_vector_pa_integrator(int dim)
{
   Mesh *mesh;
   if (dim == 2)
   {
      mesh = new Mesh(2, 2, Element::QUADRILATERAL, 0, 1.0, 1.0);
   }
   if (dim == 3)
   {
      mesh = new Mesh(2, 2, 2, Element::HEXAHEDRON, 0, 1.0, 1.0, 1.0);
   }

   int order = 2;
   H1_FECollection fec(order, dim);
   FiniteElementSpace fes(mesh, &fec, dim);

   GridFunction x(&fes), y_fa(&fes), y_pa(&fes);
   x.Randomize(1);

   BilinearForm blf_fa(&fes);
   blf_fa.AddDomainIntegrator(new INTEGRATOR);
   blf_fa.Assemble();
   blf_fa.Finalize();
   blf_fa.Mult(x, y_fa);

   BilinearForm blf_pa(&fes);
   blf_pa.SetAssemblyLevel(AssemblyLevel::PARTIAL);
   blf_pa.AddDomainIntegrator(new INTEGRATOR);
   blf_pa.Assemble();
   blf_pa.Mult(x, y_pa);

   y_fa -= y_pa;
   double difference = y_fa.Norml2();

   delete mesh;
   return difference;
}

TEST_CASE("PA Vector Mass", "[PartialAssembly], [VectorPA]")
{
   SECTION("2D")
   {
      REQUIRE(test_vector_pa_integrator<VectorMassIntegrator>(2) == Approx(0.0));
   }

   SECTION("3D")
   {
      REQUIRE(test_vector_pa_integrator<VectorMassIntegrator>(3) == Approx(0.0));
   }
}

TEST_CASE("PA Vector Diffusion", "[PartialAssembly], [VectorPA]")
{
   SECTION("2D")
   {
      REQUIRE(test_vector_pa_integrator<VectorDiffusionIntegrator>(2) == Approx(0.0));
   }

   SECTION("3D")
   {
      REQUIRE(test_vector_pa_integrator<VectorDiffusionIntegrator>(3) == Approx(0.0));
   }
}

}// namespace pa_kernels
