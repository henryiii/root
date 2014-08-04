// @(#)root/treeviewer:$Id$
// Author: Rene Brun   21/09/2010

/*************************************************************************
 * Copyright (C) 1995-2010, Rene Brun and Fons Rademakers.               *
 * All rights reserved.                                                  *
 *                                                                       *
 * For the licensing terms see $ROOTSYS/LICENSE.                         *
 * For the list of contributors see $ROOTSYS/README/CREDITS.             *
 *************************************************************************/

//___________________________________________________________________________
// Utility class post-processing the file generated by TMemStat (default memstat.root)
//
// TMemStat records all the calls to malloc and free and write a TTree
// with the position where the memory is allocated/freed , as well as
// the number of bytes.
//
// To use the class TMemStat, add the following statement at the beginning
// of your script or program
//     TMemStat mm("gnubuiltin");
// or in an interactive session do something like:
//    root > TMemStat mm("gnubuiltin");
//    root > .x somescript.C
//    root > .q
//
// another (may be more practical way) is to modify $ROOTSYS/etc/system.rootrc
// and activate the variable
//    Root.TMemStat:           1
//
// The file collected by TMemStat is named memstat_ProcessID and can be analyzed and results shown
// by executing the static function Show.
// When TMemStat is active it recors every call to malloc/free in a ROOT Tree.
// You must be careful when running jobs with many millions (or more) of calls
// to malloc/free because the generated Tree may become very large.
// The TMemStat constructor TMemStat(const char* system, Int_t buffersize, Int_t maxcalls)
// has its 3 arguments optional:
//   -system refers to the internal algorithm to compute the back traces.
//    the recommended value is "gnubuiltin"
//   -buffersize is the number of calls to malloc or free that can be stored in one memory buffer.
//    when the buffer is full, the calls to malloc/free pointing to the same location
//    are eliminated and not written to the final Tree. The default value 100000
//    is such that between 50 and 90% of the calls are eliminated depending on the application.
//    You can set buffersize <=1 to keep every single call to malloc/free.
//   -maxcalls can set a limit for the maximum number of calls to be registered in the Tree.
//    The default value is 5000000.
// The 3 arguments can be set  in $ROOTSYS/etc/system.rootrc
//    Root.TMemStat.system      gnubuiltin
//    Root.TMemStat.buffersize  100000
//    Root.TMemStat.maxcalls    5000000
//
// TMemStat::Show creates 3 canvases.
// -In canvas1 it displays a dynamic histogram showing for pages (10 kbytes by default)
//  the percentage of the page used.
//  A summary pave shows the total memory still in use when the TMemStat object
//  goes out of scope and the average occupancy of the pages.
//  The average occupancy gives a good indication of the memory fragmentation.
//  When moving the mouse on this canvas, a tooltip shows the backtrace for the allocations
//  at the address at the mouse position.
//
// -In canvas2 it displays the histogram of memory leaks in decreasing order.
//  when moving the mouse on this canvas, a tooltip shows the backtrace for the leak
//  in the bin below the mouse.
//
// -In canvas3 it displays the histogram of the nbigleaks largest leaks (default is 20)
//    for each leak, the number of allocs and average alloc size is shown.
//
//
// Simply do:
//   root > TMemStat::Show()
// or specifying arguments
//   root > TMemStat::Show(0.1,20,"mydir/mymemstat.root");
//
// The first argument to Show is the percentage of the time of the original job
// that produced the file after which the display is updated. By default update=0.1,
// ie 10 time intervals will be shown.
// The second argument is nbigleaks. if <=0 canvas2 and canvas3 are not shown
// The third argument is the imput file name (result of TMemStat).
// If this argument is omitted, Show will take the most recent file
// generated by TMemStat.
//
// You can restrict the address range to be analyzed via TMemStatShow::SetAddressRange
// You can restrict the entry range to be analyzed via TMemStatShow::SetEntryRange
//
//Author: Rene Brun 7 July 2010
//___________________________________________________________________________

#include "TMemStatShow.h"
#include "TMath.h"
#include "TFile.h"
#include "TTree.h"
#include "TCanvas.h"
#include "TStyle.h"
#include "TH1.h"
#include "TPaveText.h"
#include "TPaveLabel.h"
#include "TSystem.h"
#include "TGClient.h"
#include "TGToolTip.h"
#include "TRootCanvas.h"

   static MemInfo_t minfo;

   TTree     *TMemStatShow::fgT = 0;         //TMemStat Tree
   TH1D      *TMemStatShow::fgHalloc = 0;    //histogram with allocations
   TH1D      *TMemStatShow::fgHfree = 0;     //histogram with frees
   TH1D      *TMemStatShow::fgH = 0;         //histogram with allocations - frees
   TH1I      *TMemStatShow::fgHleaks = 0;    //histogram with leaks
   TH1I      *TMemStatShow::fgHentry = 0;    //histogram with entry numbers in the TObjArray
   TH1I      *TMemStatShow::fgHdiff = 0;     //histogram with diff of entry number between alloc/free

   TGToolTip *TMemStatShow::fgTip1 = 0;      //pointer to tool tip for canvas 1
   TGToolTip *TMemStatShow::fgTip2 = 0;      //pointer to tool tip for canvas 2
   TObjArray *TMemStatShow::fgBtidlist = 0;  //list of back trace ids
   Double_t  *TMemStatShow::fgV1 = 0;        //pointer to V1 array of TTree::Draw (pos)
   Double_t  *TMemStatShow::fgV2 = 0;        //pointer to V2 array of TTree::Draw (nbytes)
   Double_t  *TMemStatShow::fgV3 = 0;        //pointer to V3 array of TTree::Draw (time)
   Double_t  *TMemStatShow::fgV4 = 0;        //pointer to V4 array of TTree::Draw (btid)
   TCanvas   *TMemStatShow::fgC1 = 0;        //pointer to canvas showing allocs/deallocs vs time
   TCanvas   *TMemStatShow::fgC2 = 0;        //pointer to canvas with leaks in decreasing order
   TCanvas   *TMemStatShow::fgC3 = 0;        //pointer to canvas showing the main leaks

   Long64_t TMemStatShow::fgEntryFirst   = 0; //first address to process
   Long64_t TMemStatShow::fgEntryN       = 0; //number of addresses in bytes to process
   Long64_t TMemStatShow::fgAddressFirst = 0; //first entry to process
   Long64_t TMemStatShow::fgAddressN     = 0; //number of entries to process

//___________________________________________________________________________
void TMemStatShow::SetAddressRange(Long64_t nbytes, Long64_t first)
{
   //specify a memory address range to process (static function).
   // This function can be used to restrict the range of memory addresses
   // to be analyzed. For example whem TmemStat is run on a 64 bits machine and
   // the results visualized on a 32 bits machine, it might be necessary to
   // restrict the analysis range to the addresses below 2 Gigabytes, eg
   //   TMemStatShow::SetMemoryRange(500000000,0); //analyse only the first 500 MBytes
   // -first : first address to process (default is 0)
   // -nbytes : number of addresses in bytes to process starting at first
   //             if 0 (default), then all addresses are processed

   fgAddressFirst = first;
   fgAddressN     = nbytes;
}

//___________________________________________________________________________
void TMemStatShow::SetEntryRange(Long64_t nentries, Long64_t first)
{
   //specify a range of entries to process (static function)
   // -first : first entry to process (default is 0)
   // -nentries : number of entries to process starting at first
   //             if 0 (default), then all entries are processed
   // call this function when the amount of data collected in the Tree is large
   // and therefore making the analysis slow.

   fgEntryFirst = first;
   fgEntryN     = nentries;
}

//___________________________________________________________________________
void TMemStatShow::Show(double update, int nbigleaks, const char* fname)
{
   // function called by TMemStat::Show
   // Open the memstat data file, then call TTree::Draw to precompute
   // the arrays of positions and nbytes per entry.
   // update is the time interval in the data file  in seconds after which
   // the display is updated. For example is the job producing the memstat.root file
   // took 100s to execute, an update of 0.1s will generate 1000 time views of
   // the memory use.
   // the histogram hbigleaks will contain the nbigleaks largest leaks
   // if fname=="*" (default), the most recent file memstat*.root will be taken.


   TString s;
   if (!fname || strlen(fname) <5 || strstr(fname,"*")) {
      //take the most recent file memstat*.root
      s = gSystem->GetFromPipe("ls -lrt memstat*.root");
      Int_t ns = s.Length();
      fname = strstr(s.Data()+ns-25,"memstat");
   }
   printf("Analyzing file: %s\n",fname);
   TFile *f = TFile::Open(fname);
   if (!f) {
      printf("Cannot open file %s\n",fname);
      return;
   }
   fgT = (TTree*)f->Get("T");
   if (!fgT) {
      printf("cannot find the TMemStat TTree named T in file %s\n",fname);
      return;
   }
   if (update <= 0) {
      printf("Illegal update value %g, changed to 0.01\n",update);
      update = 0.01;
   }
   if (update < 0.001) printf("Warning update parameter is very small, processing may be slow\n");

   //autorestrict the amount of data to analyze
   gSystem->GetMemInfo(&minfo);
   Int_t nfree = minfo.fMemTotal - minfo.fMemUsed;  //in Mbytes
   printf("TMemStat::Show info: you are running on a machine with %d free MBytes of memory\n",nfree);
   Long64_t nfreebytes = 200000*Long64_t(nfree); //use only 20% of the memory available
   if (fgAddressN <=0) fgAddressN = nfreebytes;
   Long64_t nentries = fgT->GetEntries();
   if (fgEntryN > 0 && nentries > fgEntryN) nentries = fgEntryN;
   if (2*8*nentries > 4*nfreebytes) {
      nentries = 4*nfreebytes/16;
      printf("not enough memory, restricting analysis to %lld entries\n",nentries);
   }
   fgT->SetEstimate(nentries);
   Long64_t nsel = fgT->Draw("pos","pos>0","goff",nentries);
   fgV1 = fgT->GetV1();
   Long64_t ivmin = (Long64_t)TMath::MinElement(nsel,fgV1);
   Long64_t ivmax = (Long64_t)TMath::MaxElement(nsel,fgV1);
   if (ivmax-ivmin > fgAddressN) ivmax = ivmin+fgAddressN;
   printf("TMemStatShow::Show will analyze only %lld bytes in its first pass\n",ivmax);


   //initialize statics
   fgTip1 = 0;
   fgTip2 = 0;
   fgBtidlist = 0;

   Long64_t ne = nfreebytes/32LL;
   if (ne < nentries) nentries = ne;
   fgT->SetEstimate(nentries+10);
   printf("sel: ivmin=%lld, ivmax=%lld, nentries=%lld\n",ivmin,ivmax,nentries);
   nsel = fgT->Draw("pos:nbytes:time:btid",
      TString::Format("pos>%g && pos<%g",Double_t(ivmin),Double_t(ivmax)),
      "goff",nentries,fgEntryFirst);

   //now we compute the best binning for the histogram
   Int_t nbytes;
   Double_t pos;
   fgV1 = fgT->GetV1();
   fgV2 = fgT->GetV2();
   fgV3 = fgT->GetV3();
   fgV4 = fgT->GetV4();
   ivmin = (Long64_t)TMath::MinElement(nsel,fgV1);
   ivmax = (Long64_t)TMath::MaxElement(nsel,fgV1);
   Long64_t bw = 1000;
   Double_t dvv = (Double_t(ivmax) - Double_t(ivmin))/Double_t(bw);
   Long64_t nbins = Long64_t(dvv);
   ivmin = ivmin -ivmin%bw;
   ivmax = ivmin+bw*nbins;
   Long64_t nvm = Long64_t(ivmax-ivmin+1);
   printf("==>The data Tree contains %lld entries with addresses in range[%lld,%lld]\n",nsel,ivmin,ivmax);
   //ne = (1000000*nfree-nvm*12)/32;
   ne = 1000000LL*nfree/32LL;
   if (ne < 0) return;
   if (ne < nentries) {
      //we take only the first side of the allocations
      //we are mostly interested by the small allocations, so we select
      //only values in the first gigabyte
      nsel = fgT->Draw("pos:nbytes:time:btid",
         TString::Format("pos>=%g && pos<%g",Double_t(ivmin),Double_t(ivmax)),"goff",ne,fgEntryFirst);
      fgV1 = fgT->GetV1();
      fgV2 = fgT->GetV2();
      fgV3 = fgT->GetV3();
      fgV4 = fgT->GetV4();
      ivmin = (Long64_t)TMath::MinElement(nsel,fgV1);
      ivmax = (Long64_t)TMath::MaxElement(nsel,fgV1);
      bw = 10000;
      dvv = (Double_t(ivmax) - Double_t(ivmin))/Double_t(bw);
      nbins = Long64_t(dvv+0.5);
      ivmin = ivmin -ivmin%bw;
      ivmax = ivmin+bw*nbins;
      printf("==>Address range or/and Entry range is too large\n");
      printf("==>restricting the analysis range to [%lld,%lld] and %lld entries\n",ivmin,ivmax,ne);
      printf("==>you can restrict the address range with TMemStatShow::SetAddressRange\n");
      printf("==>you can restrict the entries range with TMemStatShow::SetEntryRange\n");
   }
   update *= 0.0001*fgV3[nsel-1]; //convert time per cent in seconds
   nvm = Long64_t(ivmax-ivmin);
   Long64_t *nbold = new Long64_t[nvm];
   Int_t *ientry  = new Int_t[nvm];
   if (!nbold || !ientry) {
      printf("you do not have enough memory to run, %lld bytes needed\n",12*nvm);
      return;
   }
   memset(nbold,0,nvm*8);
   memset(ientry,0,nvm*4);
   Double_t dv = (ivmax-ivmin)/nbins;
   TH1D *h = new TH1D("h",Form("%s;pos;per cent of pages used",fname),nbins,ivmin,ivmax);
   fgH = h;
   TAxis *axis = h->GetXaxis();
   gStyle->SetOptStat("ie");
   h->SetFillColor(kRed);
   h->SetMinimum(0);
   h->SetMaximum(100);
   fgHalloc = new TH1D("fgHalloc",Form("%s;pos;number of mallocs",fname),nbins,ivmin,ivmax);
   fgHfree  = new TH1D("fgHfree", Form("%s;pos;number of frees",fname),nbins,ivmin,ivmax);
   fgHdiff = new TH1I("fgHdiff","",1000,0,1e5);
   //open a canvas and draw the empty histogram
   fgC1 = new TCanvas("fgC1","c1",1200,600);
   fgC1->SetFrameFillColor(kYellow-3);
   fgC1->SetGridx();
   fgC1->SetGridy();
   h->Draw();
   //create a TPaveText to show the summary results
   TPaveText *pvt = new TPaveText(.5,.9,.75,.99,"brNDC");
   pvt->Draw();
   //create a TPaveLabel to show the time
   TPaveLabel *ptime = new TPaveLabel(.905,.7,.995,.76,"time","brNDC");
   ptime->SetFillColor(kYellow-3);
   ptime->Draw();
   //draw producer identifier
   TNamed *named = (TNamed*)fgT->GetUserInfo()->FindObject("SysInfo");
   TText tmachine;
   tmachine.SetTextSize(0.02);
   tmachine.SetNDC();
   if (named) tmachine.DrawText(0.01,0.01,named->GetTitle());

   //start loop on selected rows
   Int_t bin,nb=0,j;
   Long64_t ipos;
   Double_t dbin,rest,time;
   Double_t updateLast = 0;
   Int_t nleaks = 0;
   Int_t i;
   for (i=0;i<nsel;i++) {
      pos    = fgV1[i];
      ipos = (Long64_t)(pos-ivmin);
      nbytes = (Int_t)fgV2[i];
      time = 0.0001*fgV3[i];
      bin = axis->FindBin(pos);
      if (bin<1 || bin>nbins) continue;
      dbin = axis->GetBinUpEdge(bin)-pos;
      if (nbytes > 0) {
         ientry[ipos] = i;
         fgHalloc->Fill(pos);
         if (dbin > nbytes) dbin = nbytes;
         //fill bytes in the first page
         h->AddBinContent(bin,100*dbin/dv);
         //fill bytes in full following pages
         nb = Int_t((nbytes-dbin)/dv);
         if (bin+nb >nbins) nb = nbins-bin;
         for (j=1;j<=nb;j++) h->AddBinContent(bin+j,100);
         //fill the bytes remaining in last page
         rest = nbytes-nb*dv-dbin;
         if (rest > 0) h->AddBinContent(bin+nb+1,100*rest/dv);
         //we save nbytes at pos. This info will be used when we free this slot
         //if (nbold[ipos] > 0) printf("reallocating %d bytes (was %lld) at %lld, entry=%d\n",nbytes,nbold[ipos],ipos,i);
         if (nbold[ipos] == 0) {
            nleaks++;
            //save the Tree entry number where we made this allocation
            ientry[ipos] = i;
         }
         nbold[ipos] = nbytes;
      } else {
         fgHfree->Fill(pos);
         nbytes = nbold[ipos];
         if (bin+nb >nbins) nb = nbins-bin;
         nbold[ipos] = 0; nleaks--;
         fgHdiff->Fill(i-ientry[ipos]);
         if (nbytes <= 0) continue;
         //fill bytes free in the first page
         if (dbin > nbytes) dbin = nbytes;
         h->AddBinContent(bin,-100*dbin/dv);
         //fill bytes free in full following pages
         nb = Int_t((nbytes-dbin)/dv);
         if (bin+nb >nbins) nb = nbins-bin;
         for (j=1;j<=nb;j++) h->AddBinContent(bin+j,-100);
         //fill the bytes free in  in last page
         rest = nbytes-nb*dv-dbin;
         if (rest > 0) h->AddBinContent(bin+nb+1,-100*rest/dv);

      }
      if (time -updateLast > update) {
         //update canvas at regular intervals
         updateLast = time;
         h->SetEntries(i);
         fgC1->Modified();
         pvt->GetListOfLines()->Delete();
         Double_t mbytes = 0;
         Int_t nonEmpty = 0;
         Double_t w;
         for (Int_t k=1;k<nbins;k++) {
            w = h->GetBinContent(k);
            if (w > 0) {
               nonEmpty++;
               mbytes += 0.01*w*dv;
            }
         }
         Double_t occupancy = mbytes/(nonEmpty*0.01*dv);
         pvt->AddText(Form("memory used = %g Mbytes",mbytes*1e-6));
         pvt->AddText(Form("page occupancy = %f per cent",occupancy));
         pvt->AddText("(for non empty pages only)");
         ptime->SetLabel(Form("%g sec",time));

         fgC1->Update();
         gSystem->ProcessEvents();
      }
   }
   h->SetEntries(nsel);
   if (nleaks < 0) nleaks=0;
   Int_t nlmax = nleaks;
   nleaks += 1000;
   Int_t *lindex  = new Int_t[nleaks];
   Int_t *entry   = new Int_t[nleaks];
   Int_t *ileaks  = new Int_t[nleaks];

   nleaks =0;
   for (Int_t ii=0;ii<nvm;ii++) {
      if (nbold[ii] > 0) {
         ileaks[nleaks] = (Int_t)nbold[ii];
         entry[nleaks]  = ientry[ii];
         nleaks++;
         if (nleaks > nlmax) break;
      }
   }
   TMath::Sort(nleaks,ileaks,lindex);
   fgHentry = new TH1I("fgHentry","leak entry index",nleaks,0,nleaks);
   fgHleaks = new TH1I("fgHleaks","leaks;leak number;nbytes in leak",nleaks,0,nleaks);
   for (Int_t k=0;k<nleaks;k++) {
      Int_t kk = lindex[k];
      i = entry[kk];
      fgHentry->SetBinContent(k+1,i);
      fgHleaks->SetBinContent(k+1,ileaks[kk]);
   }
   delete [] ileaks;
   delete [] entry;
   delete [] lindex;
   delete [] nbold;
   delete [] ientry;
   fgHentry->SetEntries(nleaks);
   fgHleaks->SetEntries(nleaks);


   //construct the first tooltip
   fgC1->Modified();
   fgC1->Update();
   TRootCanvas *rc1 = (TRootCanvas *)fgC1->GetCanvasImp();
   TGMainFrame *frm1 = dynamic_cast<TGMainFrame *>(rc1);
   // create the tooltip with a timeout of 250 ms
   if (!fgTip1) fgTip1 = new TGToolTip(gClient->GetDefaultRoot(), frm1, "", 250);
   fgC1->Connect("ProcessedEvent(Int_t, Int_t, Int_t, TObject*)",
                "TMemStatShow", 0, "EventInfo1(Int_t, Int_t, Int_t, TObject*)");
   if (nbigleaks <= 0) return;

   //---------------------------------------------------------------------------
   //open a second canvas and draw the histogram with leaks in decreasing order
   fgC2 = new TCanvas("fgC2","c2",1200,600);
   fgC2->SetFrameFillColor(kCyan-6);
   fgC2->SetGridx();
   fgC2->SetGridy();
   fgC2->SetLogy();
   fgHleaks->SetFillColor(kRed-3);
   if (nleaks > 1000) fgHleaks->GetXaxis()->SetRange(1,1000);
   fgHleaks->Draw();
   //draw producer identifier
   if (named) tmachine.DrawText(0.01,0.01,named->GetTitle());

   //construct the second tooltip
   TRootCanvas *rc2 = (TRootCanvas *)fgC2->GetCanvasImp();
   TGMainFrame *frm2 = dynamic_cast<TGMainFrame *>(rc2);
   // create the tooltip with a timeout of 250 ms
   if (!fgTip2) fgTip2 = new TGToolTip(gClient->GetDefaultRoot(), frm2, "", 250);
   fgC2->Connect("ProcessedEvent(Int_t, Int_t, Int_t, TObject*)",
               "TMemStatShow", 0, "EventInfo2(Int_t, Int_t, Int_t, TObject*)");

   //---------------------------------------------------------------------------
   //open a third canvas and draw the histogram with the nbigleaks largest leaks
   fgC3 = new TCanvas("fgC3","c3",1200,600);
   fgC3->SetFrameFillColor(kCyan-6);
   fgC3->SetGridx();
   fgC3->SetGridy();
   fgC3->SetLogx();
   fgC3->SetLeftMargin(0.05);
   fgC3->SetRightMargin(0.7);

   //fill histogram htotleaks accumulating in the same bin all leaks
   //from btids having identical nchar first characters
   TH1I *htotleaks = new TH1I("htotleaks","main leaks sorted by btids",100,0,0);
   Int_t l;
   for (l=1;l<=nleaks;l++) {
      TString btstring = "";
      TMemStatShow::FillBTString(l,1,btstring);
      htotleaks->Fill(btstring.Data()+2,fgHleaks->GetBinContent(l));
   }
   Double_t tsize = 0.03;
   if (nbigleaks > 30) tsize = 0.02;
   htotleaks->LabelsOption(">");
   htotleaks->GetXaxis()->SetRange(1,nbigleaks);
   htotleaks->GetXaxis()->SetLabelSize(tsize);
   htotleaks->GetYaxis()->SetLabelSize(tsize);
   htotleaks->SetFillColor(kBlue-3);
   htotleaks->Draw("hbar2 y+");

   //now loop on all the sorted bins and count the number of leaks
   Double_t xr = 0.96*fgC3->GetLeftMargin();
   Double_t xr2 = 1.04*fgC3->GetLeftMargin();
   Double_t ytop = 1-fgC3->GetTopMargin();
   Double_t ylow = fgC3->GetBottomMargin();
   Double_t dy = (ytop-ylow)/nbigleaks;
   TString btstring;
   TText tnl;
   tnl.SetNDC();
   tnl.SetTextSize(tsize);
   tnl.SetTextAlign(32);
   TText tnl2;
   tnl2.SetNDC();
   tnl2.SetTextSize(tsize);
   tnl2.SetTextAlign(12);
   tnl2.SetTextColor(kYellow);
   for (Int_t lb=1;lb<=nbigleaks;lb++) {
      if (htotleaks->GetBinContent(lb) <= 0) continue;
      const char *label = htotleaks->GetXaxis()->GetBinLabel(lb);
      Int_t nchlabel = strlen(label);
      if (nchlabel == 0) htotleaks->GetXaxis()->SetBinLabel(lb,"???");
      Int_t nl =0;
      for (l=1;l<=nleaks;l++) {
         btstring = "";
         TMemStatShow::FillBTString(l,1,btstring);
         if (nchlabel > 0) {
            if (!strncmp(btstring.Data()+2,label,nchlabel)) nl++;
         } else {
            if (btstring.Length() == 0) nl++;
         }
      }
      Double_t yr = ylow +(lb-0.5)*dy;
      tnl.DrawText(xr,yr,Form("%d",nl));
      Int_t nbmean = Int_t(htotleaks->GetBinContent(lb)/nl);
      if (lb == 1) tnl2.DrawText(xr2,yr,Form("%d bytes/alloc",nbmean));
      else         tnl2.DrawText(xr2,yr,Form("%d",nbmean));
   }
   tnl.DrawText(xr,ytop+0.015,"nallocs");
   tnl.DrawText(1-fgC3->GetRightMargin(),0.5*ylow,"nbytes");
   //draw producer identifier
   if (named) tmachine.DrawText(0.01,0.01,named->GetTitle());

}

//______________________________________________________________________
void TMemStatShow::EventInfo1(Int_t event, Int_t px, Int_t , TObject *selected)
{
   // static: draw the tooltip showing the backtrace for the allocatios histogram
   if (!fgTip1) return;
   fgTip1->Hide();
   if (event == kMouseLeave)
      return;
   Double_t xpx  = fgC1->AbsPixeltoX(px);
   Double_t xpx1 = fgC1->AbsPixeltoX(px+1);
   Int_t bin  = fgH->GetXaxis()->FindBin(xpx);
   Int_t bin1 = fgH->GetXaxis()->FindBin(xpx1);
   //to take into account consecutive bins on the same pixel
   while (bin <= bin1) {
      if (fgH->GetBinContent(bin) > 0) break;
      bin++;
   }
   if (fgH->GetBinContent(bin) <= 0) return;
   if (bin <=0 || bin > fgH->GetXaxis()->GetNbins()) return;
   Double_t posmin = fgH->GetXaxis()->GetBinLowEdge(bin);
   Double_t posmax = fgH->GetXaxis()->GetBinUpEdge(bin);
   Int_t nsel   = (Int_t)fgT->GetSelectedRows();
   Int_t entry  = 0;
   Int_t nhits  = 0;
   Int_t nbytes = 0;
   //search for all allocations in this bin and select last one only
   for (Int_t i=0;i<nsel;i++) {
      if (fgV2[i] < 0) continue;
      if (fgV1[i] < posmax && fgV1[i]+fgV2[i] >posmin) {
         entry = i;
         nbytes = (Int_t)fgV2[i];
         nhits++;
      }
   }
   if (!nhits) return;

   Double_t time = 0.0001*fgV3[entry];
   TString ttip;
   TMemStatShow::FillBTString(entry,0,ttip);

   if (selected) {
      TString form1 = TString::Format("  Alloc(%d) at %lld of %d bytes, time=%gseconds\n\n",nhits,Long64_t(fgV1[entry]),nbytes,time);
      fgTip1->SetText(TString::Format("%s%s",form1.Data(),ttip.Data() ));
      fgTip1->SetPosition(px+15, 100);
      fgTip1->Reset();
   }
}

//______________________________________________________________________
void TMemStatShow::EventInfo2(Int_t event, Int_t px, Int_t , TObject *selected)
{
   // static: draw the tooltip showing the backtrace for the histogram of leaks
   if (!fgTip2) return;
   fgTip2->Hide();
   if (event == kMouseLeave)
      return;
   Double_t xpx  = fgC2->AbsPixeltoX(px);
   Int_t bin = fgHleaks->GetXaxis()->FindBin(xpx);
   if (bin <=0 || bin > fgHleaks->GetXaxis()->GetNbins()) return;
   Int_t nbytes  = (Int_t)fgHleaks->GetBinContent(bin);
   Int_t entry   = (Int_t)fgHentry->GetBinContent(bin);
   Double_t time = 0.0001*fgV3[entry];
   TString ttip;
   TMemStatShow::FillBTString(entry,0,ttip);

   if (selected) {
      TString form1 = TString::Format("  Leak number=%d, leaking %d bytes at entry=%d    time=%gseconds\n\n",bin,nbytes,entry,time);
      fgTip2->SetText(TString::Format("%s%s",form1.Data(),ttip.Data() ));
      fgTip2->SetPosition(px+15, 100);
      fgTip2->Reset();
   }
}

//______________________________________________________________________
void TMemStatShow::FillBTString(Int_t entry,Int_t mode,TString &btstring)
{
   // static: fill btstring with the traceback corresponding to entry in T
   //          btstring must be initialized in calling function

   Int_t btid   = (Int_t)fgV4[entry];
   TH1I *hbtids = (TH1I*)fgT->GetUserInfo()->FindObject("btids");
   if (!hbtids) return;
   if (!fgBtidlist) fgBtidlist = (TObjArray*)fgT->GetUserInfo()->FindObject("FAddrsList");
   if (!fgBtidlist) fgBtidlist = (TObjArray*)gFile->Get("FAddrsList"); //old memstat files
   if (!fgBtidlist) return;
   Int_t nbt = (Int_t)hbtids->GetBinContent(btid-1);
   for (Int_t i=0;i<nbt;i++) {
      Int_t j = (Int_t)hbtids->GetBinContent(btid+i);
      TNamed *nm = (TNamed*)fgBtidlist->At(j);
      if (nm==0) break;
      char *title = (char*)nm->GetTitle();
      Int_t nch = strlen(title);
      if (nch < 10) continue;
      if (strstr(title,"malloc")) continue;
      if (strstr(title,"memstat")) continue;
      if (strstr(title,"TMemStatHook")) continue;
      char *bar = strchr(title+5,'|');
      if (!bar) bar = title;

      if (strstr(bar,"operator new")) continue;
      if (strstr(bar,"libMemStat")) continue;
      if (strstr(bar,"G__Exception")) continue;
      if (mode) {
         btstring += TString::Format("%s ",bar);
         if (btstring.Length() > 80) return;
      } else {
         btstring += TString::Format("%2d %s\n",i,bar+1);
      }
   }
}
