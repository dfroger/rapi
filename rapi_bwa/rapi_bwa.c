/*
 * rapi_bwa.c
 */

#include <rapi.h>
#include <bwamem.h>
#include <kstring.h>
#include <kvec.h>
#include <utils.h>

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>


const char vtype_char[] = {
	'0',
	'A', // RAPI_VTYPE_CHAR       1
	'Z', // RAPI_VTYPE_TEXT       2
	'i', // RAPI_VTYPE_INT        3
	'f'  // RAPI_VTYPE_REAL       4
};

/**
 * The 'bwa_pg' string is statically allocated in some BWA file that we're not
 * linking, so we need to define it here.  I think it's the string that the
 * program uses to identify itself in the SAM header's @PG tag.
 */
char* bwa_pg = "rapi";

// Table to convert from base letter to an index in the range [0,4].
// Defined in bntseq.c
extern unsigned char nst_nt4_table[256];

/******** Utility functions *******/
void rapi_print_read(FILE* out, const rapi_read* read)
{
	fprintf(out, "read id: %s\n", read->id);
	fprintf(out, "read length: %d\n", read->length);
	fprintf(out, "read seq: %s\n", read->seq);
	fprintf(out, "read qual: %s\n", read->qual);
	fprintf(out, "read n_alignments: %u\n", read->n_alignments);
}


int rapi_format_tag(const rapi_tag* tag, kstring_t* str) {
	// in theory we should check the return values of all these kput functions
	// and ensure they're != EOF
	int error = RAPI_NO_ERROR;

	kputs(tag->key, str);
	kputc(':', str);
	kputc(vtype_char[tag->type], str);
	kputc(':', str);
	switch (tag->type) {
		case RAPI_VTYPE_CHAR: {
			char c;
			error = rapi_tag_get_char(tag, &c);
			if (error) return RAPI_TYPE_ERROR;
			kputc(c, str);
			break;
		 }
		case RAPI_VTYPE_TEXT: {
			const kstring_t* s;
			error = rapi_tag_get_text(tag, &s);
			if (error) return RAPI_TYPE_ERROR;
			kputsn(s->s, s->l, str);
			break;
		}
		case RAPI_VTYPE_INT: {
			long i;
			error = rapi_tag_get_long(tag, &i);
			if (error) return RAPI_TYPE_ERROR;
			kputl(i, str);
			break;
		}
		case RAPI_VTYPE_REAL: {
			double d;
			error = rapi_tag_get_dbl(tag, &d);
			if (error) return RAPI_TYPE_ERROR;
			ksprintf(str, "%f", d);
			break;
		}
		default:
			err_fatal(__func__, "Unrecognized tag type id %d\n", tag->type);
			abort();
	};
	return error;
}

/*
 * Format SAM for read, given the alignment at index `which_aln`
 * (currently in rapi_read we only have a single alignment instead of a list,
 * so this should always be 0).
 */
int rapi_format_sam(const rapi_read* read, const rapi_read* mate, kstring_t* output)
{
	/**** code based on mem_aln2sam in BWA ***/
	rapi_alignment tmp_read, tmp_mate;

	if (read->n_alignments > 0)
		tmp_read = *read->alignments;
	else
		memset(&tmp_read, 0, sizeof(tmp_read));

	if (mate && mate->n_alignments > 0)
		tmp_mate = *mate->alignments;
	else
		memset(&tmp_mate, 0, sizeof(tmp_mate));

	if (mate) {
		tmp_read.paired = 1;
		tmp_mate.paired = 1;
	}

	rapi_alignment* aln = &tmp_read;
	rapi_alignment* mate_aln = &tmp_mate;

	if (!aln->mapped && mate && mate_aln->mapped) { // copy mate position to read
		aln->contig         = mate_aln->contig;
		aln->pos            = mate_aln->pos;
		aln->reverse_strand = mate_aln->reverse_strand;
	}
	else if (aln->mapped && mate && !mate_aln->mapped) { // copy read alignment to mate
		mate_aln->contig         = aln->contig;
		mate_aln->pos            = aln->pos;
		mate_aln->reverse_strand = aln->reverse_strand;
	}

	int flag = 0;
	//
	// XXX: this implementation differs from BWA's behaviour when only one read in a pair is mapped.
	// In that case, BWA copies the coordinates of the mapped read to the unmapped one.  This
	// influences the flags printed by BWA, the coordinates and also the cigar.

	flag |= (mate && !mate_aln->mapped) ? 0x8 : 0; // is mate unmapped
	flag |= (mate && mate_aln->mapped && mate_aln->reverse_strand) ? 0x20 : 0; // is mate on the reverse strand

	flag |= aln->paired ? 0x1 : 0; // is paired in sequencing
	flag |= aln->mapped ? 0 : 0x4; // is unmapped

	if (aln->mapped)
	{
		flag |= aln->prop_paired ? 0x2 : 0;
		flag |= aln->reverse_strand ? 0x10 : 0; // is on the reverse strand
		flag |= aln->secondary_aln ? 0x100 : 0; // secondary alignment
	}

	kputs(read->id, output); kputc('\t', output); // QNAME\t
	kputw((flag & 0xffff), output); kputc('\t', output); // FLAG

	if (aln->contig) { // with coordinate
		kputs(aln->contig->name, output); kputc('\t', output); // RNAME
		kputl(aln->pos, output); kputc('\t', output); // POS
		kputw(aln->mapq, output); kputc('\t', output); // MAPQ
		// XXX: BWA forces hard clipping for supplementary alignments -- i.e., additional
		// alignments that are not marked as secondary. At the moment we're only printing
		// the first primary alignment.
		rapi_put_cigar(aln->n_cigar_ops, aln->cigar_ops, 0, output);
	}
	else
		kputsn("*\t0\t0\t*", 7, output); // unmapped

	kputc('\t', output);

	// print the mate chr, position, and isize if applicable
	if (mate_aln->contig) {
		if (aln->contig == mate_aln->contig)
			kputc('=', output);
		else
			kputs(mate_aln->contig->name, output); // RNAME
		kputc('\t', output);
		kputl(mate_aln->pos, output); kputc('\t', output); // mate pos

		if (aln->mapped && (aln->contig == mate_aln->contig))
			kputl(rapi_get_insert_size(aln, mate_aln), output);
		else
			kputc('0', output);
	}
	else
		kputsn("*\t0\t0", 5, output);
	kputc('\t', output);

	// print SEQ and QUAL
	if (aln->secondary_aln) { // for secondary alignments, don't write SEQ and QUAL
		kputsn("*\t*", 3, output);
	}
	else {
		int i, begin = 0, end = read->length;
		// Trim the printed sequence -- BWA did this for supplementary alignments
		// (those after the first in the list and not labelled as secondary 0x100)
		// if (aln->n_cigar_ops > 0) {
		// 	if (which && ((p->cigar[0]&0xf) == 4 || (p->cigar[0]&0xf) == 3)) qb += p->cigar[0]>>4;
		// 	if (which && ((p->cigar[p->n_cigar-1]&0xf) == 4 || (p->cigar[p->n_cigar-1]&0xf) == 3)) qe -= p->cigar[p->n_cigar-1]>>4;
		// }
		// ks_resize(str, str->l + (qe - qb) + 1);
		int resize = output->l + read->length + 1;
		if (read->qual)
			resize += read->length + 1; // more room for the qual sequence
		ks_resize(output, resize);

		if (!aln->reverse_strand) { // the forward strand
			kputsn(read->seq, read->length, output);
			kputc('\t', output);
			if (read->qual) { // printf qual
				for (i = begin; i < end; ++i) output->s[output->l++] = read->qual[i];
				output->s[output->l] = 0;
			} else kputc('*', output);
		} else { // the reverse strand
			for (i = begin-1; i >= begin; --i) output->s[output->l++] = "TGCAN"[nst_nt4_table[(int)read->seq[i]]];
			kputc('\t', output);
			if (read->qual) { // printf qual
				for (i = begin-1; i >= begin; --i) output->s[output->l++] = read->qual[i];
				output->s[output->l] = 0;
			} else kputc('*', output);
		}
	}

	// print optional tags
	if (aln->n_cigar_ops > 0) {
		kputsn("\tNM:i:", 6, output); kputw(aln->n_mismatches, output);
		//kputsn("\tMD:Z:", 6, output); kputs((char*)(p->cigar + p->n_cigar), str);
	}

	if (aln->score >= 0) { kputsn("\tAS:i:", 6, output); kputw(aln->score, output); }

	int error = RAPI_NO_ERROR;

	for (int t = 0; t < kv_size(aln->tags); ++t) {
		kputc('\t', output);
		error = rapi_format_tag(&kv_A(aln->tags, t), output);
	}

	//if (!(aln->flag & 0x100)) { // not multi-hit
	//	for (i = 0; i < n; ++i)
	//		if (i != which && !(list[i].flag&0x100)) break;
	//	if (i < n) { // there are other primary hits; output them
	//		kputsn("\tSA:Z:", 6, str);
	//		for (i = 0; i < n; ++i) {
	//			const mem_aln_t *r = &list[i];
	//			int k;
	//			if (i == which || (list[i].flag&0x100)) continue; // proceed if: 1) different from the current; 2) not shadowed multi hit
	//			kputs(bns->anns[r->rid].name, str); kputc(',', str);
	//			kputl(r->pos+1, str); kputc(',', str);
	//			kputc("+-"[r->is_rev], str); kputc(',', str);
	//			for (k = 0; k < r->n_cigar; ++k) {
	//				kputw(r->cigar[k]>>4, str); kputc("MIDSH"[r->cigar[k]&0xf], str);
	//			}
	//			kputc(',', str); kputw(r->mapq, str);
	//			kputc(',', str); kputw(r->NM, str);
	//			kputc(';', str);
	//		}
	//	}
	//}

	return error;
}

/**********************************/


/******** Internal structures *****/
typedef struct {
	unsigned long n_bases;
	int n_reads;
	int n_reads_per_frag;
	bseq1_t* seqs;
} bwa_batch;

void _print_bwa_batch(FILE* out, const bwa_batch* read_batch)
{
	fprintf(out, "batch with %ld bases, %d reads, %d reads per fragment",
			read_batch->n_bases, read_batch->n_reads, read_batch->n_reads_per_frag);
	if (read_batch->n_reads_per_frag > 0)
		fprintf(out, " (so %d fragments)\n",	read_batch->n_reads / read_batch->n_reads_per_frag);
	else
		fprintf(out, "\n");

	fprintf(out, "=== Reads: ===\n");
	for (int r = 0; r < read_batch->n_reads; ++r) {
		const bseq1_t* bwa_read = read_batch->seqs + r;
		fprintf(out, "-=-=--=\n");
		fprintf(out, "name: %s\n", bwa_read->name);
		fprintf(out, "seq: %s\n", bwa_read->seq);
		fprintf(out, "qual %.*s\n", bwa_read->l_seq, bwa_read->qual);
	}
}

/**********************************/

/**
 * Definition of the aligner state structure.
 */
struct rapi_aligner_state {
	int64_t n_reads_processed;
	// paired-end stats
	mem_pestat_t pes[4];
};



#if 1 // strdup is not defined if we compile with c99, but is when I include kvec.h.
// Is there a preprocessor DEFINE I can use to test whether a function is defined?
char* strdup(const char* str)
{
	if (NULL == str)
		return NULL;

	size_t len = strlen(str);
	char* new_str = malloc(len + 1);
	if (NULL == new_str)
		return NULL;
	return strcpy(new_str, str);
}
#endif

/* Init Library */
int rapi_init(const rapi_opts* opts)
{
	// no op
	return RAPI_NO_ERROR;
}

/* Init Library Options */
int rapi_opts_init( rapi_opts * my_opts )
{
	// create a BWA opt structure
	mem_opt_t*const bwa_opt = mem_opt_init();
	if (NULL == bwa_opt)
		return RAPI_MEMORY_ERROR;

	// Default values copied from bwamem.c in 0.7.8
	bwa_opt->flag = 0;
	bwa_opt->a = 1; bwa_opt->b = 4;
	bwa_opt->o_del = bwa_opt->o_ins = 6;
	bwa_opt->e_del = bwa_opt->e_ins = 1;
	bwa_opt->w = 100;
	bwa_opt->T = 30;
	bwa_opt->zdrop = 100;
	bwa_opt->pen_unpaired = 17;
	bwa_opt->pen_clip5 = bwa_opt->pen_clip3 = 5;
	bwa_opt->min_seed_len = 19;
	bwa_opt->split_width = 10;
	bwa_opt->max_occ = 10000;
	bwa_opt->max_chain_gap = 10000;
	bwa_opt->max_ins = 10000;
	bwa_opt->mask_level = 0.50;
	bwa_opt->XA_drop_ratio = 0.50;
	bwa_opt->split_factor = 1.5;
	bwa_opt->chunk_size = 10000000;
	bwa_opt->n_threads = 1;
	bwa_opt->max_matesw = 100;
	bwa_opt->mask_level_redun = 0.95;
	bwa_opt->mapQ_coef_len = 50; bwa_opt->mapQ_coef_fac = log(bwa_opt->mapQ_coef_len);
	bwa_fill_scmat(bwa_opt->a, bwa_opt->b, bwa_opt->mat);

	my_opts->_private = bwa_opt; // hand the bwa structure onto the external one
	my_opts->ignore_unsupported = 1;
	my_opts->mapq_min     = 0;
	my_opts->isize_min    = 0;
	my_opts->isize_max    = bwa_opt->max_ins;
	kv_init(my_opts->parameters);

	return RAPI_NO_ERROR;
}

int rapi_opts_free( rapi_opts * my_opts )
{
	free(my_opts->_private);
	return RAPI_NO_ERROR;
}

const char* rapi_aligner_name()
{
	return "bwa-mem";
}

const char* rapi_aligner_version()
{
	/* TODO: change build system to extract PACKAGE_VERSION macro from main.c in BWA project */
	return "0.7.8-r455";
}

/* Load Reference */
int rapi_ref_load( const char * reference_path, rapi_ref * ref_struct )
{
	if ( NULL == ref_struct || NULL == reference_path )
		return RAPI_PARAM_ERROR;

	const bwaidx_t*const bwa_idx = bwa_idx_load(reference_path, BWA_IDX_ALL);
	if ( NULL == bwa_idx )
		return RAPI_GENERIC_ERROR;

	// allocate memory
	ref_struct->path = strdup(reference_path);
	ref_struct->contigs = calloc( bwa_idx->bns->n_seqs, sizeof(rapi_contig) );
	if ( NULL == ref_struct->path || NULL == ref_struct->contigs )
	{
		// if either allocations we free everything and return an error
		bwa_idx_destroy((bwaidx_t*)bwa_idx);
		free(ref_struct->path);
		free(ref_struct->contigs);
		memset(ref_struct, 0, sizeof(*ref_struct));
		return RAPI_MEMORY_ERROR;
	}

	/* Fill in Contig Information */
	ref_struct->n_contigs = bwa_idx->bns->n_seqs; /* Handle contains bnt_seq_t * bns holding contig information */
	for ( int i = 0; i < ref_struct->n_contigs; ++i )
	{
		rapi_contig* c = &ref_struct->contigs[i];
		c->len = bwa_idx->bns->anns[i].len;
		c->name = bwa_idx->bns->anns[i].name; // points to BWA string
		c->assembly_identifier = NULL;
		c->species = NULL;
		c->uri = NULL;
		c->md5 = NULL;
	}
	ref_struct->_private = (bwaidx_t*)bwa_idx;

	return RAPI_NO_ERROR;
}

/* Free Reference */
int rapi_ref_free( rapi_ref * ref )
{
	// free bwa's part
	bwa_idx_destroy(ref->_private);

	// then free the rest of the structure
	free(ref->path);

	if (ref->contigs) {
		for ( int i = 0; i < ref->n_contigs; ++i )
		{
			rapi_contig* c = &ref->contigs[i];
			// *Don't* free name since it points to BWA's string
			free(c->assembly_identifier);
			free(c->species);
			free(c->uri);
			free(c->md5);
		}
		free (ref->contigs);
	}
	memset(ref, 0, sizeof(*ref));
	return RAPI_NO_ERROR;
}

void _free_bwa_batch_contents(bwa_batch* batch)
{
	for (int i = 0; i < batch->n_reads; ++i) {
		// *Don't* free the read id since it points to the original structure's string
		free(batch->seqs[i].seq);
		free(batch->seqs[i].qual);
	}

	free(batch->seqs);
	memset(batch, 0, sizeof(bwa_batch));
}


static int _batch_to_bwa_seq(const rapi_batch* batch, const rapi_opts* opts, bwa_batch* bwa_seqs)
{
	bwa_seqs->n_bases = 0;
	bwa_seqs->n_reads = 0;
	bwa_seqs->n_reads_per_frag = batch->n_reads_frag;
	fprintf(stderr, "Need to allocate %d elements of size %ld\n", batch->n_frags * batch->n_reads_frag, sizeof(bseq1_t));

	bwa_seqs->seqs = calloc(batch->n_frags * batch->n_reads_frag, sizeof(bseq1_t));
	if (NULL == bwa_seqs->seqs) {
		fprintf(stderr, "Allocation failed!\n");
		return RAPI_MEMORY_ERROR;
	}

	for (int f = 0; f < batch->n_frags; ++f)
	{
		for (int r = 0; r < batch->n_reads_frag; ++r)
		{
			const rapi_read* rapi_read = rapi_get_read(batch, f, r);
			bseq1_t* bwa_read = bwa_seqs->seqs + bwa_seqs->n_reads;

			// -- In bseq1_t, all strings are null-terminated.
			// We duplicated the seq and qual since BWA modifies them.
			bwa_read->seq = strdup(rapi_read->seq);
			bwa_read->qual = (NULL == rapi_read->qual) ? NULL : strdup(rapi_read->qual);
			if (!bwa_read->seq || (!rapi_read->qual && bwa_read->qual))
				goto failed_allocation;

			bwa_read->name = rapi_read->id;
			bwa_read->l_seq = rapi_read->length;
			bwa_read->comment = NULL;
			bwa_read->sam = NULL;

			// As we build the objects n_reads keeps the actual number
			// constructed thus far and thus can be used to free the allocated
			// structres in case of error.
			bwa_seqs->n_reads += 1;
			bwa_seqs->n_bases += rapi_read->length;
		}
	}
	return RAPI_NO_ERROR;

failed_allocation:
	fprintf(stderr, "Failed to allocate while constructing sequences! Freeing and returning\n");
	_free_bwa_batch_contents(bwa_seqs);
	return RAPI_MEMORY_ERROR;
}

/*
 * Many of the default option values need to be adjusted if the matching score
 * (opt->a) is changed.  This function (from the BWA code) does that.
 *
 * \param opt The structure containing actual values and that will be "fixed"
 * \param override set the members to 1 if the value has been overridden and thus should be kept;
 *                 else the corresponding value in opts is assumed to be at default and will be adjusted.
 *
 * So, if you change 'a' set the 'override' accordingly and call this function.  E.g.,
 *
 * <pre>
 *   rapi_opts* opt = rapi_init_opts();
 *   mem_opt_t* bwa_opts = (mem_opt_t*) opt->_private;
 *   bwa_opts->a = 2;
 *   bwa_opts->b = 5;
 *   mem_opt_t override;
 *   override.a = override.b = 1;
 *   adjust_bwa_opts(bwa_opts, &override);
 * </pre>
 */
void adjust_bwa_opts(mem_opt_t* opt, const mem_opt_t* override)
{
	if (override->a == 1) { // matching score is changed
		if (override->b != 1) opt->b *= opt->a;
		if (override->T != 1) opt->T *= opt->a;
		if (override->o_del != 1) opt->o_del *= opt->a;
		if (override->e_del != 1) opt->e_del *= opt->a;
		if (override->o_ins != 1) opt->o_ins *= opt->a;
		if (override->e_ins != 1) opt->e_ins *= opt->a;
		if (override->zdrop != 1) opt->zdrop *= opt->a;
		if (override->pen_clip5 != 1) opt->pen_clip5 *= opt->a;
		if (override->pen_clip3 != 1) opt->pen_clip3 *= opt->a;
		if (override->pen_unpaired != 1) opt->pen_unpaired *= opt->a;
		bwa_fill_scmat(opt->a, opt->b, opt->mat);
	}
}

static int _convert_opts(const rapi_opts* opts, mem_opt_t* bwa_opts)
{
	bwa_opts->T = opts->mapq_min;
	bwa_opts->max_ins = opts->isize_max;

	// TODO: other options provided through 'parameters' field
	return RAPI_NO_ERROR;
}

int rapi_aligner_state_init(const rapi_opts* opts, struct rapi_aligner_state** ret_state)
{
	// allocate and zero the structure
	rapi_aligner_state* state = *ret_state = calloc(1, sizeof(rapi_aligner_state));
	if (NULL == state)
		return RAPI_MEMORY_ERROR;
	return RAPI_NO_ERROR;
}

int rapi_aligner_state_free(rapi_aligner_state* state)
{
	free(state);
	return RAPI_NO_ERROR;
}

void rapi_put_cigar(int n_ops, const rapi_cigar* ops, int force_hard_clip, kstring_t* output)
{
	if (n_ops > 0) {
		for (int i = 0; i < n_ops; ++i) {
			int c = ops[i].op;
			if (c == 3 || c == 4) c = force_hard_clip ? 4 : 3;
			kputw(ops[i].len, output);
			kputc("MIDSH"[c], output);
		}
	}
	else
		kputc('*', output);
}

long rapi_get_insert_size(const rapi_alignment* read, const rapi_alignment* mate)
{
	long isize = 0;

	if (read->mapped && mate->mapped && (read->contig == mate->contig))
	{
		if (mate->n_cigar_ops == 0 || read->n_cigar_ops == 0)
			err_fatal(__func__, "No cigar ops for mapped reads! aln->n_cigar_ops: %d; mate_aln->n_cigar_ops: %d\n", read->n_cigar_ops, mate->n_cigar_ops);

		int64_t p0 = read->pos + (read->reverse_strand ? rapi_get_rlen(read->n_cigar_ops, read->cigar_ops) - 1 : 0);
		int64_t p1 = mate->pos + (mate->reverse_strand ? rapi_get_rlen(mate->n_cigar_ops, mate->cigar_ops) - 1 : 0);
		isize = -(p0 - p1 + (p0 > p1? 1 : p0 < p1? -1 : 0));
	}
	return isize;
}


/********** modified BWA code *****************/

/* Copied directly from bwamem_pair */
inline int mem_infer_dir(int64_t l_pac, int64_t b1, int64_t b2, int64_t *dist)
{
	int64_t p2;
	int r1 = (b1 >= l_pac), r2 = (b2 >= l_pac);
	p2 = r1 == r2? b2 : (l_pac<<1) - 1 - b2; // p2 is the coordinate of read 2 on the read 1 strand
	*dist = p2 > b1? p2 - b1 : b1 - p2;
	return (r1 == r2? 0 : 1) ^ (p2 > b1? 0 : 3);
}


extern mem_alnreg_v mem_align1_core(const mem_opt_t *opt, const bwt_t *bwt, const bntseq_t *bns, const uint8_t *pac, int l_seq, char *seq);
// IMPORTANT: must run mem_sort_and_dedup() before calling mem_mark_primary_se function (but it's called by mem_align1_core)
void mem_mark_primary_se(const mem_opt_t *opt, int n, mem_alnreg_t *a, int64_t id);

/* based on mem_aln2sam */
static int _bwa_aln_to_rapi_aln(const rapi_ref* rapi_ref, rapi_read* our_read, int is_paired,
		const bseq1_t *s,
		const mem_aln_t *const bwa_aln_list, int list_length)
{
	if (list_length < 0)
		return RAPI_PARAM_ERROR;

	our_read->alignments = calloc(list_length, sizeof(rapi_alignment));
	if (NULL == our_read->alignments)
		return RAPI_MEMORY_ERROR;
	our_read->n_alignments = list_length;

	for (int which = 0; which < list_length; ++which)
	{
		const mem_aln_t* bwa_aln = &bwa_aln_list[which];
		rapi_alignment* our_aln = &our_read->alignments[which];

		if (bwa_aln->rid >= rapi_ref->n_contigs) { // huh?? Out of bounds
			fprintf(stderr, "read reference id value %d is out of bounds (n_contigs: %d)\n", bwa_aln->rid, rapi_ref->n_contigs);
			free(our_read->alignments);
			our_read->alignments = NULL; our_read->n_alignments = 0;
			return RAPI_GENERIC_ERROR;
		}

		// set flags
		our_aln->paired = is_paired != 0;
		// TODO: prop_paired:  how does BWA define a "proper pair"?
		our_aln->prop_paired = 0;
		our_aln->score = bwa_aln->score;
		our_aln->mapq = bwa_aln->mapq;
		// In BWA's code (e.g., mem_aln2sam) when the 0x10000 bit is set the alignment
		// is printed as a secondary alignment (i.e., the 0x100 bit is set in the flag).
		our_aln->secondary_aln = ((bwa_aln->flag & 0x100) | (bwa_aln->flag & 0x10000)) != 0;

		our_aln->mapped = bwa_aln->rid >= 0;
		if (bwa_aln->rid >= 0) { // with coordinate
			our_aln->reverse_strand = bwa_aln->is_rev != 0;
			our_aln->contig = &rapi_ref->contigs[bwa_aln->rid];
			our_aln->pos = bwa_aln->pos + 1;
			our_aln->n_mismatches = bwa_aln->NM;
			if (bwa_aln->n_cigar) { // aligned
				our_aln->cigar_ops = malloc(bwa_aln->n_cigar * sizeof(our_aln->cigar_ops[0]));
				if (NULL == our_aln->cigar_ops)
					err_fatal(__func__, "Failed to allocate cigar space");
				our_aln->n_cigar_ops = bwa_aln->n_cigar;
				for (int i = 0; i < bwa_aln->n_cigar; ++i) {
					our_aln->cigar_ops[i].op = bwa_aln->cigar[i] & 0xf;
					our_aln->cigar_ops[i].len = bwa_aln->cigar[i] >> 4;
				}
			}
		}

		rapi_tag tag;

		if (bwa_aln->sub >= 0) {
			rapi_tag_set_key(&tag, "XS");
			rapi_tag_set_text(&tag, "");
			kputw(bwa_aln->sub, &tag.value.text);
			kv_push(rapi_tag, our_aln->tags, tag);
		}

		// TODO: extra tags
		/* this section outputs other primary hits in the SA tag
		if (!(bwa_aln->flag & 0x100)) { // not multi-hit
			for (i = 0; i < n; ++i)
				if (i != which && !(bwa_aln_list[i].flag&0x100)) break;
			if (i < n) { // there are other primary hits; output them
				kputsn("\tSA:Z:", 6, str);
				for (i = 0; i < n; ++i) {
					const mem_aln_t *r = &bwa_aln_list[i];
					int k;
					if (i == which || (bwa_aln_list[i].flag&0x100)) continue; // proceed if: 1) different from the current; 2) not shadowed multi hit
					kputs(bns->anns[r->rid].name, str); kputc(',', str);
					kputl(r->pos+1, str); kputc(',', str);
					kputc("+-"[r->is_rev], str); kputc(',', str);
					for (k = 0; k < r->n_cigar; ++k) {
						kputw(r->cigar[k]>>4, str); kputc("MIDSH"[r->cigar[k]&0xf], str);
					}
					kputc(',', str); kputw(r->mapq, str);
					kputc(',', str); kputw(r->NM, str);
					kputc(';', str);
				}
			}
		}
		*/
	}
	return RAPI_NO_ERROR;
}

/*
 * Based on mem_reg2sam_se.
 * We took out the call to mem_aln2sam and instead write the result to
 * the corresponding rapi_read structure.
 */
static int _bwa_reg2_rapi_aln_se(const mem_opt_t *opt, const rapi_ref* rapi_ref, rapi_read* our_read, bseq1_t *seq, mem_alnreg_v *a, int extra_flag, const mem_aln_t *m)
{
	int error = RAPI_NO_ERROR;
	const bntseq_t *const bns = ((bwaidx_t*)rapi_ref->_private)->bns;
	const uint8_t *const pac = ((bwaidx_t*)rapi_ref->_private)->pac;

	kvec_t(mem_aln_t) aa;
	int k;

	kv_init(aa);
	for (k = 0; k < a->n; ++k) {
		mem_alnreg_t *p = &a->a[k];
		mem_aln_t *q;
		if (p->score < opt->T) continue;
		if (p->secondary >= 0 && !(opt->flag&MEM_F_ALL)) continue;
		if (p->secondary >= 0 && p->score < a->a[p->secondary].score * .5) continue;
		q = kv_pushp(mem_aln_t, aa);
		*q = mem_reg2aln(opt, bns, pac, seq->l_seq, seq->seq, p);
		q->flag |= extra_flag; // flag secondary
		if (p->secondary >= 0) q->sub = -1; // don't output sub-optimal score
		if (k && p->secondary < 0) // if supplementary
			q->flag |= (opt->flag&MEM_F_NO_MULTI)? 0x10000 : 0x800;
		if (k && q->mapq > aa.a[0].mapq) q->mapq = aa.a[0].mapq;
	}
	if (aa.n == 0) { // no alignments good enough; then write an unaligned record
		mem_aln_t t;
		t = mem_reg2aln(opt, bns, pac, seq->l_seq, seq->seq, 0);
		t.flag |= extra_flag;
		// RAPI
		error = _bwa_aln_to_rapi_aln(rapi_ref, our_read, 0, seq, &t, 1);
	}
	else {
		error = _bwa_aln_to_rapi_aln(rapi_ref, our_read, /* unpaired */ 0, seq, /* list of aln */ aa.a, aa.n);
	}

	if (aa.n > 0)
	{
		for (int k = 0; k < aa.n; ++k)
			free(aa.a[k].cigar);
		free(aa.a);
	}
	return error;
}


#if 1
#define raw_mapq(diff, a) ((int)(6.02 * (diff) / (a) + .499))
/*
 * Mostly taken from mem_sam_pe in bwamem_pair.c
 */
int _bwa_mem_pe(const mem_opt_t *opt, const rapi_ref* rapi_ref, const mem_pestat_t pes[4], uint64_t id, bseq1_t s[2], mem_alnreg_v a[2], rapi_read out[2])
{
	// functions defined in bwamem.c or bwamem_pair.c
	extern void mem_mark_primary_se(const mem_opt_t *opt, int n, mem_alnreg_t *a, int64_t id);
	extern int mem_approx_mapq_se(const mem_opt_t *opt, const mem_alnreg_t *a);
	extern int mem_matesw(const mem_opt_t *opt, int64_t l_pac, const uint8_t *pac, const mem_pestat_t pes[4], const mem_alnreg_t *a, int l_ms, const uint8_t *ms, mem_alnreg_v *ma);
	extern int mem_pair(const mem_opt_t *opt, int64_t l_pac, const uint8_t *pac, const mem_pestat_t pes[4], bseq1_t s[2], mem_alnreg_v a[2], int id, int *sub, int *n_sub, int z[2]);

	const bntseq_t *const bns = ((bwaidx_t*)rapi_ref->_private)->bns;
	const uint8_t *const pac = ((bwaidx_t*)rapi_ref->_private)->pac;

	int n = 0, i, j, z[2], o, subo, n_sub, extra_flag = 1;
	kstring_t str;
	mem_aln_t h[2];

	str.l = str.m = 0; str.s = 0;
	if (!(opt->flag & MEM_F_NO_RESCUE)) { // then perform SW for the best alignment
		mem_alnreg_v b[2];
		kv_init(b[0]); kv_init(b[1]);
		for (i = 0; i < 2; ++i)
			for (j = 0; j < a[i].n; ++j)
				if (a[i].a[j].score >= a[i].a[0].score  - opt->pen_unpaired)
					kv_push(mem_alnreg_t, b[i], a[i].a[j]);
		for (i = 0; i < 2; ++i)
			for (j = 0; j < b[i].n && j < opt->max_matesw; ++j)
				n += mem_matesw(opt, bns->l_pac, pac, pes, &b[i].a[j], s[!i].l_seq, (uint8_t*)s[!i].seq, &a[!i]);
		free(b[0].a); free(b[1].a);
	}
	mem_mark_primary_se(opt, a[0].n, a[0].a, id<<1|0);
	mem_mark_primary_se(opt, a[1].n, a[1].a, id<<1|1);
	if (opt->flag&MEM_F_NOPAIRING) goto no_pairing;
	// pairing single-end hits
	if (a[0].n && a[1].n && (o = mem_pair(opt, bns->l_pac, pac, pes, s, a, id, &subo, &n_sub, z)) > 0) {
		int is_multi[2], q_pe, score_un, q_se[2];
		// check if an end has multiple hits even after mate-SW
		for (i = 0; i < 2; ++i) {
			for (j = 1; j < a[i].n; ++j)
				if (a[i].a[j].secondary < 0 && a[i].a[j].score >= opt->T) break;
			is_multi[i] = j < a[i].n? 1 : 0;
		}
		if (is_multi[0] || is_multi[1]) goto no_pairing; // TODO: in rare cases, the true hit may be long but with low score
		// compute mapQ for the best SE hit
		score_un = a[0].a[0].score + a[1].a[0].score - opt->pen_unpaired;
		//q_pe = o && subo < o? (int)(MEM_MAPQ_COEF * (1. - (double)subo / o) * log(a[0].a[z[0]].seedcov + a[1].a[z[1]].seedcov) + .499) : 0;
		subo = subo > score_un? subo : score_un;
		q_pe = raw_mapq(o - subo, opt->a);
		if (n_sub > 0) q_pe -= (int)(4.343 * log(n_sub+1) + .499);
		if (q_pe < 0) q_pe = 0;
		if (q_pe > 60) q_pe = 60;
		// the following assumes no split hits
		if (o > score_un) { // paired alignment is preferred
			mem_alnreg_t *c[2];
			c[0] = &a[0].a[z[0]]; c[1] = &a[1].a[z[1]];
			for (i = 0; i < 2; ++i) {
				if (c[i]->secondary >= 0)
					c[i]->sub = a[i].a[c[i]->secondary].score, c[i]->secondary = -2;
				q_se[i] = mem_approx_mapq_se(opt, c[i]);
			}
			q_se[0] = q_se[0] > q_pe? q_se[0] : q_pe < q_se[0] + 40? q_pe : q_se[0] + 40;
			q_se[1] = q_se[1] > q_pe? q_se[1] : q_pe < q_se[1] + 40? q_pe : q_se[1] + 40;
			extra_flag |= 2;
			// cap at the tandem repeat score
			q_se[0] = q_se[0] < raw_mapq(c[0]->score - c[0]->csub, opt->a)? q_se[0] : raw_mapq(c[0]->score - c[0]->csub, opt->a);
			q_se[1] = q_se[1] < raw_mapq(c[1]->score - c[1]->csub, opt->a)? q_se[1] : raw_mapq(c[1]->score - c[1]->csub, opt->a);
		} else { // the unpaired alignment is preferred
			z[0] = z[1] = 0;
			q_se[0] = mem_approx_mapq_se(opt, &a[0].a[0]);
			q_se[1] = mem_approx_mapq_se(opt, &a[1].a[0]);
		}

		// write SAM
		h[0] = mem_reg2aln(opt, bns, pac, s[0].l_seq, s[0].seq, &a[0].a[z[0]]); h[0].mapq = q_se[0]; h[0].flag |= 0x40 | extra_flag;
		h[1] = mem_reg2aln(opt, bns, pac, s[1].l_seq, s[1].seq, &a[1].a[z[1]]); h[1].mapq = q_se[1]; h[1].flag |= 0x80 | extra_flag;
		// RAPI: instead of writing sam, convert mem_aln_t into our alignments
		// XXX: I'm not so sure the alignment I'm passing in.  Review
		int error1 = _bwa_aln_to_rapi_aln(rapi_ref, &out[0], 1, &s[0], &h[0], 1);
		int error2 = _bwa_aln_to_rapi_aln(rapi_ref, &out[1], 1, &s[1], &h[1], 1);
		if (error1 || error2) {
			err_fatal(__func__, "error %d while converting BWA mem_aln_t for read %d into rapi alignments\n", (error1 ? 1 : 2), (error1 ? error1 : error2));
			abort();
		}

		if (strcmp(s[0].name, s[1].name) != 0) err_fatal(__func__, "paired reads have different names: \"%s\", \"%s\"\n", s[0].name, s[1].name);
		free(h[0].cigar); free(h[1].cigar);

	} else goto no_pairing;
	return n;

no_pairing:
	for (i = 0; i < 2; ++i) {
		if (a[i].n && a[i].a[0].score >= opt->T)
			h[i] = mem_reg2aln(opt, bns, pac, s[i].l_seq, s[i].seq, &a[i].a[0]);
		else h[i] = mem_reg2aln(opt, bns, pac, s[i].l_seq, s[i].seq, 0);
	}
	if (!(opt->flag & MEM_F_NOPAIRING) && h[0].rid == h[1].rid && h[0].rid >= 0) { // if the top hits from the two ends constitute a proper pair, flag it.
		int64_t dist;
		int d;
		d = mem_infer_dir(bns->l_pac, a[0].a[0].rb, a[1].a[0].rb, &dist);
		if (!pes[d].failed && dist >= pes[d].low && dist <= pes[d].high) extra_flag |= 2;
	}
	h[0].flag |= 0x41|extra_flag;
	h[1].flag |= 0x81|extra_flag;

	int error1 = _bwa_reg2_rapi_aln_se(opt, rapi_ref, &out[0], &s[0], &a[0], 0x41|extra_flag, &h[1]);
	int error2 = _bwa_reg2_rapi_aln_se(opt, rapi_ref, &out[1], &s[1], &a[1], 0x81|extra_flag, &h[0]);
	if (error1 || error2) {
		err_fatal(__func__, "error %d while converting *with no pairing* BWA mem_aln_t for read %d into rapi alignments\n", (error1 ? 1 : 2), (error1 ? error1 : error2));
		abort();
	}

	if (strcmp(s[0].name, s[1].name) != 0) err_fatal(__func__, "paired reads have different names: \"%s\", \"%s\"\n", s[0].name, s[1].name);
	free(h[0].cigar); free(h[1].cigar);
	return n;
}

typedef struct {
	const mem_opt_t *opt;
	const rapi_ref* rapi_ref;
	const bwa_batch* read_batch;
	rapi_read* rapi_reads; // need to pass these along because the code to convert BWA alignments into rapi is nested pretty deep
	mem_pestat_t *pes;
	mem_alnreg_v *regs;
	int64_t n_processed;
} bwa_worker_t;

/*
 * This function is the same as worker1 from bwamem.c
 */
static void bwa_worker_1(void *data, int i, int tid)
{
	bwa_worker_t *w = (bwa_worker_t*)data;

	const bwaidx_t* const bwaidx = (bwaidx_t*)(w->rapi_ref->_private);
	const bwt_t*    const bwt    = bwaidx->bwt;
	const bntseq_t* const bns    = bwaidx->bns;
	const uint8_t*  const pac    = bwaidx->pac;

	fprintf(stderr, "bwa_worker_1: MEM_F_PE is %sset\n", ((w->opt->flag & MEM_F_PE) == 0 ? "not " : " "));
	if (w->opt->flag & MEM_F_PE) {
		int read = 2*i;
		int mate = 2*i + 1;
		w->regs[read] = mem_align1_core(w->opt, bwt, bns, pac, w->read_batch->seqs[read].l_seq, w->read_batch->seqs[read].seq);
		w->regs[mate] = mem_align1_core(w->opt, bwt, bns, pac, w->read_batch->seqs[mate].l_seq, w->read_batch->seqs[mate].seq);
	} else {
		w->regs[i] = mem_align1_core(w->opt, bwt, bns, pac, w->read_batch->seqs[i].l_seq, w->read_batch->seqs[i].seq);
	}
}

/* based on worker2 from bwamem.c */
static void bwa_worker_2(void *data, int i, int tid)
{
	bwa_worker_t *w = (bwa_worker_t*)data;
	fprintf(stderr, "bwa_worker_2 with i %d\n", i);
	int error = RAPI_NO_ERROR;

	if ((w->opt->flag & MEM_F_PE)) {
		// paired end
		//mem_sam_pe(w->opt, w->bns, w->pac, w->pes, (w->n_processed>>1) + i, &w->seqs[i<<1], &w->regs[i<<1]);
		error = _bwa_mem_pe(w->opt, w->rapi_ref, w->pes, w->n_processed / 2 + i, &(w->read_batch->seqs[2 * i]), &w->regs[2 * i], &(w->rapi_reads[2 * i]));
		free(w->regs[2 * i].a); free(w->regs[2 * i + 1].a);
	}
	else {
		// single end
		mem_mark_primary_se(w->opt, w->regs[i].n, w->regs[i].a, w->n_processed + i);
		//mem_reg2sam_se(w->opt, w->bns, w->pac, &w->seqs[i], &w->regs[i], 0, 0);
		//error = _mem_reg2_rapi_aln_se(w->opt, w->rapi_ref, &(w->read_batch->seqs[i]), &w->regs[i], &(w->rapi_reads[i]), 0, 0);
		free(w->regs[i].a);
	}

	if (error != RAPI_NO_ERROR)
		err_fatal(__func__, "error %d while running %s end alignments\n", error, ((w->opt->flag & MEM_F_PE) ? "pair" : "single"));
}

#endif
/********** end modified BWA code *****************/

int rapi_align_reads( const rapi_ref* ref,  rapi_batch * batch, const rapi_opts * config, rapi_aligner_state* state )
{
	int error = RAPI_NO_ERROR;

	if (batch->n_reads_frag > 2)
		return RAPI_OP_NOT_SUPPORTED_ERROR;

	if (batch->n_reads_frag <= 0)
		return RAPI_PARAM_ERROR;

	// "extract" BWA-specific structures
	mem_opt_t*const bwa_opt = (mem_opt_t*) config->_private;

	if (batch->n_reads_frag == 2) // paired-end
		bwa_opt->flag |= MEM_F_PE;

	if ((error = _convert_opts(config, bwa_opt)))
		return error;
	fprintf(stderr, "opts converted\n");

	// traslate our read structure into BWA reads
	bwa_batch bwa_seqs;
	if ((error = _batch_to_bwa_seq(batch, config, &bwa_seqs)))
		return error;
	fprintf(stderr, "converted reads to BWA structures.\n");

	_print_bwa_batch(stderr, &bwa_seqs);

	fprintf(stderr, "Going to process.\n");
	mem_alnreg_v *regs = malloc(bwa_seqs.n_reads * sizeof(mem_alnreg_v));
	if (NULL == regs) {
		error = RAPI_MEMORY_ERROR;
		goto clean_up;
	}
	fprintf(stderr, "Allocated %d mem_alnreg_v structures\n", bwa_seqs.n_reads);

	extern void kt_for(int n_threads, void (*func)(void*,int,int), void *data, int n);
	bwa_worker_t w;
	w.opt = bwa_opt;
	w.read_batch = &bwa_seqs;
	w.regs = regs;
	w.pes = state->pes;
	w.n_processed = state->n_reads_processed;
	w.rapi_ref = ref;
	w.rapi_reads = batch->reads;

	fprintf(stderr, "Calling bwa_worker_1. bwa_opt->flag: %d\n", bwa_opt->flag);
	int n_fragments = (bwa_opt->flag & MEM_F_PE) ? bwa_seqs.n_reads / 2 : bwa_seqs.n_reads;
	kt_for(bwa_opt->n_threads, bwa_worker_1, &w, n_fragments); // find mapping positions

	if (bwa_opt->flag & MEM_F_PE) { // infer insert sizes if not provided
		// TODO: support manually setting insert size dist parameters
		// if (pes0) memcpy(pes, pes0, 4 * sizeof(mem_pestat_t)); // if pes0 != NULL, set the insert-size distribution as pes0
		mem_pestat(bwa_opt, ((bwaidx_t*)ref->_private)->bns->l_pac, bwa_seqs.n_reads, regs, w.pes); // infer the insert size distribution from data
	}
	kt_for(bwa_opt->n_threads, bwa_worker_2, &w, n_fragments); // generate alignment

	// run the alignment
	state->n_reads_processed += bwa_seqs.n_reads;
	fprintf(stderr, "processed %lld reads\n", state->n_reads_processed);

clean_up:
	free(regs);
	_free_bwa_batch_contents(&bwa_seqs);

	return error;
}

/******* Generic functions ***********
 * These are generally usable. We should put them in a generic rapi.c file.
 *************************************/

/* Allocate reads */
int rapi_reads_alloc( rapi_batch * batch, int n_reads_fragment, int n_fragments )
{
	if (n_fragments < 0 || n_reads_fragment < 0)
		return RAPI_PARAM_ERROR;

	batch->reads = calloc( n_reads_fragment * n_fragments, sizeof(rapi_read) );
	if (NULL == batch->reads)
		return RAPI_MEMORY_ERROR;
	batch->n_frags = n_fragments;
	batch->n_reads_frag = n_reads_fragment;
	return RAPI_NO_ERROR;
}

int rapi_reads_reserve(rapi_batch * batch, int n_fragments)
{
	if (n_fragments < 0)
		return RAPI_PARAM_ERROR;
	if (n_fragments == 0)
		return RAPI_NO_ERROR;

	if (n_fragments > batch->n_frags)
	{
		// Current space insufficient.  Need to reallocate.
		int old_n_reads = batch->n_frags * batch->n_reads_frag;
		int new_n_reads = n_fragments * batch->n_reads_frag;
		rapi_read* space = realloc(batch->reads, new_n_reads * sizeof(batch->reads[0]));
		if (space == NULL)
			return RAPI_MEMORY_ERROR;
		else {
			memset(space + old_n_reads, 0, (new_n_reads - old_n_reads) * sizeof(batch->reads[0]));
			batch->n_frags = n_fragments;
			batch->reads = space;
		}
	}
	return RAPI_NO_ERROR;
}

int rapi_reads_free( rapi_batch * batch )
{
	for (int f = 0; f < batch->n_frags; ++f) {
		for (int r = 0; r < batch->n_reads_frag; ++r) {
			rapi_read* read = rapi_get_read(batch, f, r);
			// the reads use a single chunk of memory for id, seq and quality
			free(read->id);
			for (int a = 0; a < read->n_alignments; ++a) {
				for (int t = 0; t < read->alignments[a].tags.n; ++t)
					rapi_tag_clear(&read->alignments[a].tags.a[t]);
				kv_destroy(read->alignments[a].tags);
				free(read->alignments[a].cigar_ops);
			}
			free(read->alignments);
			read->n_alignments = 0;
			// *Don't* free the contig name.  It belongs to the contig structure.
		}
	}

	free(batch->reads);
	memset(batch, 0, sizeof(*batch));

	return RAPI_NO_ERROR;
}

int rapi_set_read(rapi_batch* batch,
			int n_frag, int n_read,
			const char* name, const char* seq, const char* qual,
			int q_offset) {
	int error_code = RAPI_NO_ERROR;

	if (n_frag < 0 || n_frag >= batch->n_frags
	 || n_read < 0 || n_read >= batch->n_reads_frag)
		return RAPI_PARAM_ERROR;

	rapi_read* read = rapi_get_read(batch, n_frag, n_read);
	const int name_len = strlen(name);
	const int seq_len = strlen(seq);
	read->length = seq_len;

	// simplify allocation and error checking by allocating a single buffer
	int buf_size = name_len + 1 + seq_len + 1;
	if (qual)
		buf_size += seq_len + 1;

	read->id = malloc(buf_size);
	if (NULL == read->id) { // failed allocation
		fprintf(stderr, "Unable to allocate memory for sequence\n");
		return RAPI_MEMORY_ERROR;
	}

	// copy name
	strcpy(read->id, name);

	// sequence, placed right after the name
	read->seq = read->id + name_len + 1;
	strcpy(read->seq, seq);

	// the quality, if we have it, may need to be recoded
	if (NULL == qual)
		read->qual = NULL;
	else {
		read->qual = read->seq + seq_len + 1;
		for (int i = 0; i < seq_len; ++i) {
			read->qual[i] = (int)qual[i] - q_offset + 33; // 33 is the Sanger offset.  BWA expects it this way.
			if (read->qual[i] > 127)
			{ // qual is unsigned, by Sanger base qualities have an allowed range of [0,94], and 94+33=127
				fprintf(stderr, "Invalid base quality score %d\n", read->qual[i]);
				error_code = RAPI_PARAM_ERROR;
				goto error;
			}
		}
		read->qual[seq_len] = '\0';
	}

	// trim from the name /[12]$
	int t = name_len;
	if (t > 2 && read->id[t-2] == '/' && (read->id[t-1] == '1' || read->id[t-1] == '2'))
		read->id[t-2] = '\0';

	return RAPI_NO_ERROR;

error:
	// In case of error, free any allocated memory and return the error
	free(read->id);
	return error_code;
}

