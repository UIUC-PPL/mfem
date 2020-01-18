//                       MFEM Example 23 - Parallel Version
//
// Compile with: make ex23p
//
// Sample runs:  mpirun -np 4 ex23p -m ../../data/square-disc.mesh -o 2
//               mpirun -np 4 ex23p -m ../../data/beam-tet.mesh
//               mpirun -np 4 ex23p -m ../../data/beam-hex.mesh
//               mpirun -np 4 ex23p -m ../../data/fichera.mesh
//               mpirun -np 4 ex23p -m ../../data/amr-quad.mesh -o 2
//               mpirun -np 4 ex23p -m ../../data/amr-hex.mesh
//               mpirun -np 4 ex23p -m ../../hexa728.mesh
//               mpirun -np 4 ex23p -m ../../data/rectwhole7_2attr.e
// Description:  This example code solves a simple electromagnetic wave
//               propagation problem corresponding to the second order indefinite
//               Maxwell equation curl curl E - \omega^2 E = f with a PML
//               We discretize with Nedelec finite elements in 2D or 3D.
//
//               The example also demonstrates the use of complex valued bilinear and linear forms.
//               We recommend viewing example 22 before viewing this example.

#include "mfem.hpp"
#include <fstream>
#include <iostream>

#ifdef _WIN32
#define jn(n, x) _jn(n, x)
#endif

using namespace std;
using namespace mfem;

#ifndef MFEM_USE_SUPERLU
#error This example requires that MFEM is built with MFEM_USE_SUPERLU=YES
#endif

// Exact solution, E, and r.h.s., f. See below for implementation.
void compute_pml_mesh_data(Mesh *mesh);
void maxwell_ess_data(const Vector &x, std::vector<std::complex<double>> &Eval);
void E_bdr_data_Re(const Vector &x, Vector &E);
void E_bdr_data_Im(const Vector &x, Vector &E);

void source_im(const Vector &x, Vector &f_im);

double pml_detJ_inv_Re(const Vector &x);
double pml_detJ_inv_Im(const Vector &x);
void pml_detJ_JT_J_inv_Re(const Vector &x, DenseMatrix &M);
void pml_detJ_JT_J_inv_Im(const Vector &x, DenseMatrix &M);
void pml_detJ_inv_JT_J_Re(const Vector &x, DenseMatrix &M);
void pml_detJ_inv_JT_J_Im(const Vector &x, DenseMatrix &M);
void compute_pml_elem_list(ParMesh *pmesh, Array<int> &elem_pml);

double omega;
int dim;
Array2D<double> domain_bdr;
Array2D<double> pml_lngth;
Array2D<double> comp_domain_bdr;

enum prob_type
{
   load_src,
   scatter,
   waveguide,
   cyl_waveguide
};
prob_type prob = scatter;

int main(int argc, char *argv[])
{
   // 1. Initialize MPI.
   int num_procs, myid;
   MPI_Init(&argc, &argv);
   MPI_Comm_size(MPI_COMM_WORLD, &num_procs);
   MPI_Comm_rank(MPI_COMM_WORLD, &myid);

   // 2. Parse command-line options.
   const char *mesh_file = "../../data/beam-tet.mesh";
   int order = 1;
   bool visualization = 1;
   double freq = 1.0;
   int ref_levels = 1;
   int par_ref_levels = 1;
   OptionsParser args(argc, argv);
   args.AddOption(&mesh_file, "-m", "--mesh",
                  "Mesh file to use.");
   args.AddOption(&order, "-o", "--order",
                  "Finite element order (polynomial degree).");
   args.AddOption(&freq, "-f", "--frequency", "Set the frequency for the exact"
                                              " solution.");
   args.AddOption(&ref_levels, "-rs", "--refinements-serial",
                  "Number of serial refinements");
   args.AddOption(&par_ref_levels, "-rp", "--refinements-parallel",
                  "Number of parallel refinements");
   args.AddOption(&visualization, "-vis", "--visualization", "-no-vis",
                  "--no-visualization",
                  "Enable or disable GLVis visualization.");
   args.Parse();
   if (!args.Good())
   {
      if (myid == 0)
      {
         args.PrintUsage(cout);
      }
      MPI_Finalize();
      return 1;
   }
   if (myid == 0)
   {
      args.PrintOptions(cout);
   }
   omega = 2.0 * M_PI * freq;

   // 3. Read the (serial) mesh from the given mesh file on all processors.  We
   //    can handle triangular, quadrilateral, tetrahedral, hexahedral, surface
   //    and volume meshes with the same code.

   Mesh *mesh;
    prob = scatter;
   // prob = waveguide;
   // prob = cyl_waveguide;
    prob = load_src;

   if (prob == load_src)
   {
      mesh = new Mesh(mesh_file, 1, 1);
   }
   if (prob == scatter)
   {
      mesh_file = "../data/square_w_hole.mesh";
      // mesh_file = "meshes/hexa728.mesh";
      // mesh_file ="../data/square-disc.mesh";
      mesh = new Mesh(mesh_file, 1, 1);
   }
   else if (prob == waveguide)
   {
      mesh = new Mesh(8, 1, 1, Element::HEXAHEDRON, true, 8, 1, 1, false);
   }
   else if (prob == cyl_waveguide)
   {
      // mesh_file= "cylinder_h80.mesh";
      mesh_file = "cylinder_h40.mesh";
      // mesh_file= "pipe_p2.mesh";
      // mesh_file= "hollow_cyl.mesh";
      mesh = new Mesh(mesh_file, 1, 1);
   }

   dim = mesh->Dimension();
   compute_pml_mesh_data(mesh);

   int sdim = mesh->SpaceDimension();

   // 4. Refine the serial mesh on all processors to increase the resolution. In
   //    this example we do 'ref_levels' of uniform refinement. We choose
   //    'ref_levels' to be the largest number that gives a final mesh with no
   //    more than 1,000 elements.
   {
      //  (int)floor(log(1000. / mesh->GetNE()) / log(2.) / dim);
      for (int l = 0; l < ref_levels; l++)
      {
         mesh->UniformRefinement();
      }
   }

   // 5. Define a parallel mesh by a partitioning of the serial mesh. Refine
   //    this mesh further in parallel to increase the resolution. Once the
   //    parallel mesh is defined, the serial mesh can be deleted. Tetrahedral
   //    meshes need to be reoriented before we can define high-order Nedelec
   //    spaces on them.
   ParMesh *pmesh = new ParMesh(MPI_COMM_WORLD, *mesh);
   delete mesh;
   {
      for (int l = 0; l < par_ref_levels; l++)
      {
         pmesh->UniformRefinement();
      }
   }

   Array<int> elems_pml;
   compute_pml_elem_list(pmesh, elems_pml);

   // 6. Define a parallel finite element space on the parallel mesh. Here we
   //    use the Nedelec finite elements of the specified order.
   FiniteElementCollection *fec = new ND_FECollection(order, dim);
   ParFiniteElementSpace *fespace = new ParFiniteElementSpace(pmesh, fec);
   HYPRE_Int size = fespace->GlobalTrueVSize();
   if (myid == 0)
   {
      cout << "Number of finite element unknowns: " << size << endl;
   }

   // 7. Determine the list of true (i.e. parallel conforming) essential
   //    boundary dofs. In this example, the boundary conditions are defined
   //    by marking all the boundary attributes from the mesh as essential
   //    (Dirichlet) and converting them to a list of true dofs.
   Array<int> ess_tdof_list;
   Array<int> ess_bdr;
   if (pmesh->bdr_attributes.Size())
   {
      ess_bdr.SetSize(pmesh->bdr_attributes.Max());
      ess_bdr = 1;
      fespace->GetEssentialTrueDofs(ess_bdr, ess_tdof_list);
   }

   // 8. Set up the parallel linear form b(.) which corresponds to the
   //    right-hand side of the FEM linear system, which in this case is
   //    (f,phi_i) where f is given by the function f_exact and phi_i are the
   //    basis functions in the finite element fespace.
   // Right hand side is zero
   VectorFunctionCoefficient f_Im(dim, source_im);
   ParComplexLinearForm b(fespace, ComplexOperator::HERMITIAN);
   if (prob == load_src)
   {
      b.AddDomainIntegrator(NULL, new VectorFEDomainLFIntegrator(f_Im));
   }
   b.real().Vector::operator=(0.0);
   b.imag().Vector::operator=(0.0);
   b.Assemble();

   // 9. Define the solution vector x as a parallel finite element grid function
   //    corresponding to fespace. Initialize x by projecting the exact
   //    solution. Note that only values from the boundary edges will be used
   //    when eliminating the non-homogeneous boundary condition to modify the
   //    r.h.s. vector b.
   ParComplexGridFunction x(fespace);
   VectorFunctionCoefficient E_Re(sdim, E_bdr_data_Re);
   VectorFunctionCoefficient E_Im(sdim, E_bdr_data_Im);
   x.ProjectBdrCoefficientTangent(E_Re, E_Im, ess_bdr);

   // 10. Set up the parallel bilinear form corresponding to the EM diffusion
   //     operator curl muinv curl + sigma I, by adding the curl-curl and the
   //     mass domain integrators.
   ConstantCoefficient muinv(1.0);
   ConstantCoefficient sigma(-pow(omega, 2));
   FunctionCoefficient det_inv_Re(pml_detJ_inv_Re);
   FunctionCoefficient det_inv_Im(pml_detJ_inv_Im);
   MatrixFunctionCoefficient temp_c1_Re(dim, pml_detJ_inv_JT_J_Re);
   MatrixFunctionCoefficient temp_c1_Im(dim, pml_detJ_inv_JT_J_Im);
   MatrixFunctionCoefficient temp_c2_Re(dim, pml_detJ_JT_J_inv_Re);
   MatrixFunctionCoefficient temp_c2_Im(dim, pml_detJ_JT_J_inv_Im);

   ScalarMatrixProductCoefficient pml_c1_Re(muinv, temp_c1_Re);
   ScalarMatrixProductCoefficient pml_c1_Im(muinv, temp_c1_Im);
   ScalarMatrixProductCoefficient pml_c2_Re(sigma, temp_c2_Re);
   ScalarMatrixProductCoefficient pml_c2_Im(sigma, temp_c2_Im);

   ParSesquilinearForm a(fespace, ComplexOperator::HERMITIAN);

   if (dim == 3)
   {
      a.AddDomainIntegrator(new CurlCurlIntegrator(pml_c1_Re),
                            new CurlCurlIntegrator(pml_c1_Im));
   }
   else
   {
      a.AddDomainIntegrator(new CurlCurlIntegrator(det_inv_Re),
                            new CurlCurlIntegrator(det_inv_Im));
   }
   a.AddDomainIntegrator(new VectorFEMassIntegrator(pml_c2_Re),
                         new VectorFEMassIntegrator(pml_c2_Im));
   a.Assemble();

   OperatorHandle Ah;
   Vector B, X;
   a.FormLinearSystem(ess_tdof_list, x, b, Ah, X, B);

   // Transform to monolithic HypreParMatrix
   HypreParMatrix *A = Ah.As<ComplexHypreParMatrix>()->GetSystemMatrix();

   if (myid == 0)
   {
      cout << "Size of linear system: " << A->GetGlobalNumRows() << endl;
   }

   // SuperLU direct solver
   SuperLURowLocMatrix *SA = new SuperLURowLocMatrix(*A);
   SuperLUSolver *superlu = new SuperLUSolver(MPI_COMM_WORLD);
   superlu->SetPrintStatistics(false);
   superlu->SetSymmetricPattern(false);
   superlu->SetColumnPermutation(superlu::PARMETIS);
   superlu->SetOperator(*SA);
   superlu->Mult(B, X);

   // 13. Recover the parallel grid function corresponding to X. This is the
   //     local finite element solution on each processor.
   a.RecoverFEMSolution(X, b, x);

   ParComplexGridFunction x_gf(fespace);
   x_gf.ProjectCoefficient(E_Re, E_Im);

   // Compute error
   if (prob == scatter || prob == waveguide)
   {
      int order_quad = max(2, 2 * order + 1);
      const IntegrationRule *irs[Geometry::NumGeom];
      for (int i = 0; i < Geometry::NumGeom; ++i)
      {
         irs[i] = &(IntRules.Get(i, order_quad));
      }

      double L2Error_Re = x.real().ComputeL2Error(E_Re, irs, &elems_pml);
      double L2Error_Im = x.imag().ComputeL2Error(E_Im, irs, &elems_pml);

      ParComplexGridFunction x_gf0(fespace);
      x_gf0 = 0.0;
      double norm_E_Re = x_gf0.real().ComputeL2Error(E_Re, irs, &elems_pml);
      double norm_E_Im = x_gf0.imag().ComputeL2Error(E_Im, irs, &elems_pml);

      if (myid == 0)
      {
         cout << " Rel Error - Real Part: || E_h - E || / ||E|| = " << L2Error_Re / norm_E_Re << '\n'
              << endl;
         cout << " Rel Error - Imag Part: || E_h - E || / ||E|| = " << L2Error_Im / norm_E_Im << '\n'
              << endl;
         cout << " Total Error: " << sqrt(L2Error_Re * L2Error_Re + L2Error_Im * L2Error_Im)
              << endl;
      }
   }

   // 16. Send the solution by socket to a GLVis server.
   if (visualization)
   {
      string keys;
      if (dim == 3)
      {
         keys = "keys mF\n";
      }
      else
      {
         keys = "keys arRljcUU\n";
      }
      char vishost[] = "localhost";
      int visport = 19916;
      // socketstream src_sock_re(vishost, visport);
      // src_sock_re << "parallel " << num_procs << " " << myid << "\n";
      // src_sock_re.precision(8);
      // src_sock_re << "solution\n"
      //             << *pmesh << x_gf.real() << keys
      //             << "window_title 'Source real part'" << flush;

      // MPI_Barrier(MPI_COMM_WORLD);
      // socketstream src_sock_im(vishost, visport);
      // src_sock_im << "parallel " << num_procs << " " << myid << "\n";
      // src_sock_im.precision(8);
      // src_sock_im << "solution\n"
      //             << *pmesh << x_gf.imag() << keys
      //             << "window_title 'Source imag part'" << flush;

      MPI_Barrier(MPI_COMM_WORLD);
      socketstream sol_sock_re(vishost, visport);
      sol_sock_re << "parallel " << num_procs << " " << myid << "\n";
      sol_sock_re.precision(8);
      sol_sock_re << "solution\n"
                  << *pmesh << x.real() << keys
                  << "window_title 'Solution real part'" << flush;

      MPI_Barrier(MPI_COMM_WORLD);
      socketstream sol_sock_im(vishost, visport);
      sol_sock_im << "parallel " << num_procs << " " << myid << "\n";
      sol_sock_im.precision(8);
      sol_sock_im << "solution\n"
                  << *pmesh << x.imag() << keys
                  << "window_title 'Solution imag part'" << flush;

      // MPI_Barrier(MPI_COMM_WORLD);
      // ParGridFunction u_err_re(fespace);
      // subtract(x.real(),x_gf.real(), u_err_re);
      // // u_err_re -= x_gf;
      // socketstream sol_sock_err_re(vishost, visport);
      // sol_sock_err_re << "parallel " << num_procs << " " << myid << "\n";
      // sol_sock_err_re.precision(8);
      // sol_sock_err_re << "solution\n" << *pmesh << u_err_re << keys <<"window_title 'Error real part'" << flush;

      // MPI_Barrier(MPI_COMM_WORLD);
      // ParGridFunction u_err_im(fespace);
      // subtract(x.imag(),x_gf.imag(), u_err_im);
      // // u_err_im -= x_gf;
      // socketstream sol_sock_err_im(vishost, visport);
      // sol_sock_err_im << "parallel " << num_procs << " " << myid << "\n";
      // sol_sock_err_im.precision(8);
      // sol_sock_err_im << "solution\n" << *pmesh << u_err_im << keys <<"window_title 'Error real part'" << flush;

      MPI_Barrier(MPI_COMM_WORLD);
      ParGridFunction x_t(fespace);
      x_t = x.real();
      keys = "keys rRljcUUuu\n";
      socketstream sol_sock(vishost, visport);
      sol_sock << "parallel " << num_procs << " " << myid << "\n";
      sol_sock.precision(8);
      sol_sock << "solution\n"
               << *pmesh << x_t << keys << "autoscale off\n"
               // << "valuerange -4.0  4.0 \n "
               << "window_title 'Harmonic Solution (t = 0.0 T)'"
               << "pause\n"
               << flush;
      if (myid == 0)
         cout << "GLVis visualization paused."
              << " Press space (in the GLVis window) to resume it.\n";
      int num_frames = 32;
      int i = 0;
      while (sol_sock)
      {
         double t = (double)(i % num_frames) / num_frames;
         ostringstream oss;
         oss << "Harmonic Solution (t = " << t << " T)";

         add(cos(omega * t), x.real(),
             sin(omega * t), x.imag(), x_t);
         sol_sock << "parallel " << num_procs << " " << myid << "\n";
         sol_sock << "solution\n"
                  << *pmesh << x_t
                  << "window_title '" << oss.str() << "'" << flush;
         i++;
      }
   }

   // 17. Free the used memory.
   // delete superlu;
   // delete SA;
   delete fespace;
   delete fec;
   delete pmesh;

   MPI_Finalize();
   return 0;
}

void source_im(const Vector &x, Vector &f_im)
{
   // double x0 = 10.*x(0);
   // double x1 = 10.*x(1);
   // double r = sqrt(pow(x0- alpha, 2.) + pow(x1 - beta, 2.));

   Vector center(dim);
   double r = 0.0;
   for (int i=0; i<dim; ++ i) 
   {
      center(i) = 0.5*(  comp_domain_bdr(i,0) + comp_domain_bdr(i,1));
      r += pow(x[i]- center[i], 2.);
   }   

   // f_im = 0.0;
   // double coeff = 1000.0;
   // f_im[0] = omega * exp(-coeff * r*r);

   double n = 5.0 * omega/M_PI;
   double coeff = pow(n,2)/M_PI;
   double alpha = - pow(n,2) * r;

   f_im[0] =  coeff * exp(alpha);

}

void compute_pml_mesh_data(Mesh *mesh)
{
   mesh->EnsureNodes();
   GridFunction *nodes = mesh->GetNodes();
   // Assuming square/cubic domain
   int ndofs = nodes->FESpace()->GetNDofs();
   Array2D<double> coords(ndofs, dim);
   Vector xcoords(ndofs), ycoords(ndofs), zcoords(ndofs);

   for (int comp = 0; comp < nodes->FESpace()->GetVDim(); comp++)
   {
      // cout << comp << endl;
      for (int i = 0; i < ndofs; i++)
      {
         if (comp == 0)
         {
            xcoords(i) = (*nodes)[nodes->FESpace()->DofToVDof(i, comp)];
         }
         else if (comp == 1)
         {
            ycoords(i) = (*nodes)[nodes->FESpace()->DofToVDof(i, comp)];
         }
         else if (comp == 2)
         {
            zcoords(i) = (*nodes)[nodes->FESpace()->DofToVDof(i, comp)];
         }
      }
   }

   domain_bdr.SetSize(3, 2);
   domain_bdr(0, 0) = xcoords.Min();
   domain_bdr(0, 1) = xcoords.Max();
   domain_bdr(1, 0) = ycoords.Min();
   domain_bdr(1, 1) = ycoords.Max();
   domain_bdr(2, 0) = zcoords.Min();
   domain_bdr(2, 1) = zcoords.Max();

   pml_lngth.SetSize(dim, 2);
   comp_domain_bdr.SetSize(dim, 2);
   if (prob == scatter || prob == load_src)
   {
      for (int i = 0; i < dim; i++)
      {
         for (int j = 0; j < 2; j++)
         {
            pml_lngth(i, j) = 0.25 * (domain_bdr(i, 1) - domain_bdr(i, 0));
         }
         comp_domain_bdr(i, 0) = domain_bdr(i, 0) + pml_lngth(i, 0);
         comp_domain_bdr(i, 1) = domain_bdr(i, 1) - pml_lngth(i, 1);
      }
   }
   else if (prob == waveguide)
   {
      for (int i = 0; i < dim; i++)
      {
         comp_domain_bdr(i, 0) = domain_bdr(i, 0);
         comp_domain_bdr(i, 1) = domain_bdr(i, 1);
      }
      // pml only in the x direction
      pml_lngth(0, 1) = 0.25 * (domain_bdr(0, 1) - domain_bdr(0, 0));
      comp_domain_bdr(0, 1) = domain_bdr(0, 1) - pml_lngth(0, 1);
   }
   else if (prob == cyl_waveguide)
   {
      for (int i = 0; i < dim; i++)
      {
         comp_domain_bdr(i, 0) = domain_bdr(i, 0);
         comp_domain_bdr(i, 1) = domain_bdr(i, 1);
      }
      // pml only in the z direction
      pml_lngth(2, 1) = 0.125 * (domain_bdr(2, 1) - domain_bdr(2, 0));
      comp_domain_bdr(2, 1) = domain_bdr(2, 1) - pml_lngth(2, 1);

      // pml only in the x direction
      // pml_lngth(0,1) = 0.125 * (domain_bdr(0,1) - domain_bdr(0,0));
      // pml_lngth(0,1) = 1.0;
      // comp_domain_bdr(0,1) = domain_bdr(0,1) - pml_lngth(0,1);
   }

   // comp_domain_bdr.Print();
   // domain_bdr.Print();
}

void compute_pml_elem_list(ParMesh *pmesh, Array<int> &elem_pml)
{
   int nrelem = pmesh->GetNE();
   // initialize list with 1
   elem_pml.SetSize(nrelem);
   elem_pml = 1;
   // loop through the elements and identify which of them are in the pml
   pmesh->EnsureNodes();
   GridFunction *nodes = pmesh->GetNodes();
   // Assuming square/cubic domain
   int ndofs = nodes->FESpace()->GetNDofs();
   Array2D<double> coords(ndofs, dim);

   for (int comp = 0; comp < dim; comp++)
   {
      // cout << comp << endl;
      for (int i = 0; i < ndofs; i++)
      {
         coords(i, comp) = (*nodes)[nodes->FESpace()->DofToVDof(i, comp)];
      }
   }

   for (int i = 0; i < nrelem; ++i)
   {
      Element *el = pmesh->GetElement(i);
      Array<int> vertices;
      el->GetVertices(vertices);
      // get the elent
      int nrvert = vertices.Size();
      // Check if any vertex is in the pml
      bool in_pml = false;
      for (int iv = 0; iv < nrvert; ++iv)
      {
         int vert_idx = vertices[iv];
         for (int comp = 0; comp < dim; ++comp)
         {
            if (coords(vert_idx, comp) > comp_domain_bdr(comp, 1) ||
                coords(vert_idx, comp) < comp_domain_bdr(comp, 0))
            {
               in_pml = true;
               break;
            }
         }
      }
      if (in_pml)
      {
         elem_pml[i] = 0;
      }
   }
}

void maxwell_ess_data(const Vector &x, std::vector<std::complex<double>> &E)
{
   // Initialize
   for (int i = 0; i < dim; ++i)
   {
      E[i] = complex<double>(0., 0.);
   }

   std::complex<double> zi = std::complex<double>(0., 1.);

   if (prob == waveguide)
   {
      double k10 = sqrt(omega * omega - M_PI * M_PI);
      E[1] = -zi * omega / M_PI * sin(M_PI * x(2)) * exp(zi * k10 * x(0)); // T_10 mode
      // // normalize
      double Ho = omega / M_PI;
      // E[1] /= Ho;
      //   double k20 = sqrt(omega*omega - 4.0 * M_PI * M_PI);
      //   E[1]=-zi * omega / (2.0 * M_PI) * sin(2.0 * M_PI* x(2)) * exp(zi * k20 * x(0)); // T_20 mode
      //   double Ho = omega / (2.0 * M_PI);
      E[1] /= Ho;
   }
   else if (prob == cyl_waveguide)
   {
      // if (x[2] == 0.0)
      // {
      //    double r = sqrt(x(0)*x(0) + x(1)*x(1));
      //    double th = atan(x(1)/x(0));
      //    double omega_11 = 1.30;
      //    double k11 = 0.15;
      //    complex<double> E_r  =  omega / (r * omega_11 * omega_11) * j1(omega_11 * r) * exp(zi * th);
      //    double dj1 = omega_11 * (j0(omega_11 * r) - 1.0/(omega_11 * r) * j1(omega_11* r));
      //    complex<double> E_th = zi * omega / omega_11 * dj1 * exp(zi * th);
      //    E[0] = cos(th) * E_r - sin(th) * E_th;
      //    E[1] = sin(th) * E_r + cos(th) * E_th;

      //    E[0] *= exp(zi * k11 * x(2));
      //    E[1] *= exp(zi * k11 * x(2));
      // }

      //   E[1] = exp(zi * omega * x(2));
      //   E[0] = 1.0;
      //   E[1] = 1.0*exp(-2.0*pow(r,2));
      //   E[1] = 1.0;
      if (x[2] == 0.0)
      {
         E[0] = 1.0;
         E[1] = 1.0;
      }
   }
   else if (prob == scatter) // point source (scattering)
   {
      Vector shift(dim);
      shift = 0.0;
      for (int i = 0; i < dim; ++i)
      {
         shift(i) = -0.5 * (domain_bdr(i, 0) + domain_bdr(i, 1));
      }

      if (dim == 2)
      {
         double x0 = x(0) + shift(0);
         double x1 = x(1) + shift(1);
         std::complex<double> val, val_x, val_xx, val_xy;
         double r = sqrt(x0 * x0 + x1 * x1);
         double beta = omega * r;

         // Ho = Jo(r) + i Yo(r)
         // Ho_r  = - H_1 = - (J1(r) + i Y1(r))
         // Ho_rr = - (i/r *(J1(r) + i Y1(r)) - (J2(r) + i y2(r)))
         // Special functions (Bessel functions)
         complex<double> Ho = jn(0, beta) + zi * yn(0, beta);
         complex<double> Ho_r = -omega * (jn(1, beta) + zi * yn(1, beta));
         complex<double> Ho_rr = -omega * omega * (1.0 / beta * (jn(1, beta) + zi * yn(1, beta)) - (jn(2, beta) + zi * yn(2, beta)));

         // derivative with respect to x
         double r_x = x0 / r;
         double r_y = x1 / r;
         double r_xy = -(r_x / r) * r_y;
         double r_xx = (1.0 / r) * (1.0 - r_x * r_x);

         val = 0.25 * zi * Ho; // i/4 * H_0^1(omega * r)
         val_x = 0.25 * zi * r_x * Ho_r;
         val_xx = 0.25 * zi * (r_xx * Ho_r + r_x * r_x * Ho_rr);
         val_xy = 0.25 * zi * (r_xy * Ho_r + r_x * r_y * Ho_rr);
         E[0] = zi / omega * (omega * omega * val + val_xx);
         E[1] = zi / omega * val_xy;
      }
      else
      {
         double x0 = x(0) + shift(0);
         double x1 = x(1) + shift(1);
         double x2 = x(2) + shift(2);
         double r = sqrt(x0 * x0 + x1 * x1 + x2 * x2);

         double r_x = x0 / r;
         double r_y = x1 / r;
         double r_z = x2 / r;
         double r_xx = (1.0 / r) * (1.0 - r_x * r_x);
         double r_yx = -(r_y / r) * r_x;
         double r_zx = -(r_z / r) * r_x;

         complex<double> val;
         complex<double> val_x, val_y, val_z;
         complex<double> val_xx, val_yx, val_zx;
         complex<double> val_r, val_rr;

         val = exp(zi * omega * r) / r;
         val_r = val / r * (zi * omega - 1.0);
         val_rr = val / (r * r) * (-omega * omega * r * r - 2. * zi * omega * r + 2.);
         val_x = val_r * r_x;
         val_y = val_r * r_y;
         val_z = val_r * r_z;

         val_xx = val_rr * r_x * r_x + val_r * r_xx;
         val_yx = val_rr * r_x * r_y + val_r * r_yx;
         val_zx = val_rr * r_x * r_z + val_r * r_zx;

         complex<double> alpha;
         alpha = zi * omega / 4.0 / M_PI / omega / omega;
         E[0] = alpha * (omega * omega * val + val_xx);
         E[1] = alpha * val_yx;
         E[2] = alpha * val_zx;
      }
   }
}

void E_bdr_data_Re(const Vector &x, Vector &E)
{
   // Initialize
   E = 0.0;
   bool in_pml = false;
   if (prob == scatter)
   {
      for (int i = 0; i < dim; ++i)
      {
         // check if x(i) is in the computational domain or not
         if (abs(x(i) - domain_bdr(i, 0)) < 1e-13 || abs(x(i) - domain_bdr(i, 1)) < 1e-13)
         {
            in_pml = true;
            break;
         }
      }
      if (!in_pml)
      {
         std::vector<std::complex<double>> Eval(E.Size());
         maxwell_ess_data(x, Eval);
         for (int i = 0; i < dim; ++i)
         {
            E[i] = Eval[i].real();
         }
      }
   }
   else if (prob == waveguide)
   {
      // waveguide problem
      std::vector<std::complex<double>> Eval(E.Size());
      maxwell_ess_data(x, Eval);
      for (int i = 0; i < dim; ++i)
      {
         E[i] = Eval[i].real();
      }
      if (abs(x(0) - domain_bdr(0, 1)) < 1e-13)
      {
         E = 0.0;
      }
   }
   else if (prob == cyl_waveguide)
   {
      // waveguide problem
      std::vector<std::complex<double>> Eval(E.Size());
      //   if (abs(x(2)) < 1e-12)
      //   {
      maxwell_ess_data(x, Eval);
      for (int i = 0; i < dim; ++i)
         E[i] = Eval[i].real();
      // if (abs(x(2)-domain_bdr(2,1)) < 1e-12) E = 0.0;
   }
}

//define bdr_data solution
void E_bdr_data_Im(const Vector &x, Vector &E)
{
   E = 0.0;
   bool in_pml = false;
   if (prob == scatter)
   {
      for (int i = 0; i < dim; ++i)
      {
         if (abs(x(i) - domain_bdr(i, 0)) < 1e-13 || abs(x(i) - domain_bdr(i, 1)) < 1e-13)
         {
            in_pml = true;
            break;
         }
      }
      if (!in_pml)
      {
         std::vector<std::complex<double>> Eval(E.Size());
         maxwell_ess_data(x, Eval);
         for (int i = 0; i < dim; ++i)
         {
            E[i] = Eval[i].imag();
         }
      }
   }
   else if (prob == waveguide)
   {
      // waveguide problem
      std::vector<std::complex<double>> Eval(E.Size());
      maxwell_ess_data(x, Eval);
      for (int i = 0; i < dim; ++i)
      {
         E[i] = Eval[i].imag();
      }
      if (abs(x(0) - domain_bdr(0, 1)) < 1e-13)
      {
         E = 0.0;
      }
   }
   else if (prob == cyl_waveguide)
   {
      // waveguide problem
      std::vector<std::complex<double>> Eval(E.Size());
      maxwell_ess_data(x, Eval);
      for (int i = 0; i < dim; ++i)
         E[i] = Eval[i].imag();

      // if (abs(x(2)-domain_bdr(2,1)) < 1e-12) E = 0.0;
   }
}

// PML
void pml_function(const Vector &x, std::vector<std::complex<double>> &dxs)
{
   std::complex<double> zi = std::complex<double>(0., 1.);
   std::complex<double> one = std::complex<double>(1., 0.);

   double n = 2.0;
   double c = 10.0;
   double coeff;

   // initialize to one
   for (int i = 0; i < dim; ++i)
   {
      dxs[i] = one;
   }

   // Stretch in each direction independenly
   for (int i = 0; i < dim; ++i)
   {
      for (int j = 0; j < 2; ++j)
      {
         if (x(i) >= comp_domain_bdr(i, 1))
         {
            coeff = n * c / omega / pow(pml_lngth(i, 1), n);
            dxs[i] = one + zi * coeff * abs(pow(x(i) - comp_domain_bdr(i, 1), n - 1.0));
         }
         if (x(i) <= comp_domain_bdr(i, 0))
         {
            coeff = n * c / omega / pow(pml_lngth(i, 0), n);
            dxs[i] = one + zi * coeff * abs(pow(x(i) - comp_domain_bdr(i, 0), n - 1.0));
         }
      }
   }
}

double pml_detJ_inv_Re(const Vector &x)
{
   std::complex<double> one = std::complex<double>(1., 0.);
   std::vector<std::complex<double>> dxs(dim);
   complex<double> det(1.0, 0.0);
   pml_function(x, dxs);
   for (int i = 0; i < dim; ++i)
   {
      det *= dxs[i];
   }

   complex<double> det_inv = one / det;
   return det_inv.real();
}
double pml_detJ_inv_Im(const Vector &x)
{
   std::complex<double> one = std::complex<double>(1., 0.);
   std::vector<std::complex<double>> dxs(dim);
   complex<double> det(1.0, 0.0);
   pml_function(x, dxs);
   for (int i = 0; i < dim; ++i)
   {
      det *= dxs[i];
   }

   complex<double> det_inv = one / det;
   return det_inv.imag();
}
void pml_detJ_JT_J_inv_Re(const Vector &x, DenseMatrix &M)
{
   std::complex<double> one = std::complex<double>(1., 0.);
   std::vector<complex<double>> diag(dim);
   std::vector<std::complex<double>> dxs(dim);
   complex<double> det(1.0, 0.0);
   pml_function(x, dxs);

   for (int i = 0; i < dim; ++i)
   {
      diag[i] = one / pow(dxs[i], 2);
      det *= dxs[i];
   }

   M.SetSize(dim);
   M = 0.0;

   for (int i = 0; i < dim; ++i)
   {
      complex<double> temp = det * diag[i];
      M(i, i) = temp.real();
   }
}
void pml_detJ_JT_J_inv_Im(const Vector &x, DenseMatrix &M)
{
   std::complex<double> one = std::complex<double>(1., 0.);
   std::vector<std::complex<double>> diag(dim);
   std::vector<std::complex<double>> dxs(dim);
   complex<double> det = 1.0;
   pml_function(x, dxs);

   for (int i = 0; i < dim; ++i)
   {
      diag[i] = one / pow(dxs[i], 2);
      det *= dxs[i];
   }

   M.SetSize(dim);
   M = 0.0;

   for (int i = 0; i < dim; ++i)
   {
      complex<double> temp = det * diag[i];
      M(i, i) = temp.imag();
   }
}
void pml_detJ_inv_JT_J_Re(const Vector &x, DenseMatrix &M)
{
   std::vector<complex<double>> diag(dim);
   std::vector<std::complex<double>> dxs(dim);
   complex<double> det(1.0, 0.0);
   pml_function(x, dxs);

   for (int i = 0; i < dim; ++i)
   {
      diag[i] = pow(dxs[i], 2);
      det *= dxs[i];
   }

   M.SetSize(dim);
   M = 0.0;

   for (int i = 0; i < dim; ++i)
   {
      complex<double> temp = diag[i] / det;
      M(i, i) = temp.real();
   }
}
void pml_detJ_inv_JT_J_Im(const Vector &x, DenseMatrix &M)
{
   std::vector<std::complex<double>> diag(dim);
   std::vector<std::complex<double>> dxs(dim);
   complex<double> det = 1.0;
   pml_function(x, dxs);

   for (int i = 0; i < dim; ++i)
   {
      diag[i] = pow(dxs[i], 2);
      det *= dxs[i];
   }

   M.SetSize(dim);
   M = 0.0;

   for (int i = 0; i < dim; ++i)
   {
      complex<double> temp = diag[i] / det;
      M(i, i) = temp.imag();
   }
}
