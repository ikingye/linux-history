/*
   raid0.c : Multiple Devices driver for Linux
             Copyright (C) 1994-96 Marc ZYNGIER
	     <zyngier@ufr-info-p7.ibp.fr> or
	     <maz@gloups.fdn.fr>
             Copyright (C) 1999, 2000 Ingo Molnar, Red Hat


   RAID-0 management functions.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2, or (at your option)
   any later version.
   
   You should have received a copy of the GNU General Public License
   (for example /usr/src/linux/COPYING); if not, write to the Free
   Software Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.  
*/

#include <linux/module.h>
#include <linux/raid/raid0.h>

#define MAJOR_NR MD_MAJOR
#define MD_DRIVER
#define MD_PERSONALITY
#define DEVICE_NR(device) (minor(device))

static int create_strip_zones (mddev_t *mddev)
{
	int i, c, j;
	sector_t current_offset, curr_zone_offset;
	raid0_conf_t *conf = mddev_to_conf(mddev);
	mdk_rdev_t *smallest, *rdev1, *rdev2, *rdev;
	struct list_head *tmp1, *tmp2;
	struct strip_zone *zone;
	int cnt;
 
	/*
	 * The number of 'same size groups'
	 */
	conf->nr_strip_zones = 0;
 
	ITERATE_RDEV(mddev,rdev1,tmp1) {
		printk("raid0: looking at %s\n",
			bdev_partition_name(rdev1->bdev));
		c = 0;
		ITERATE_RDEV(mddev,rdev2,tmp2) {
			printk("raid0:   comparing %s(%llu) with %s(%llu)\n",
				bdev_partition_name(rdev1->bdev),
				(unsigned long long)rdev1->size,
				bdev_partition_name(rdev2->bdev),
				(unsigned long long)rdev2->size);
			if (rdev2 == rdev1) {
				printk("raid0:   END\n");
				break;
			}
			if (rdev2->size == rdev1->size)
			{
				/*
				 * Not unique, don't count it as a new
				 * group
				 */
				printk("raid0:   EQUAL\n");
				c = 1;
				break;
			}
			printk("raid0:   NOT EQUAL\n");
		}
		if (!c) {
			printk("raid0:   ==> UNIQUE\n");
			conf->nr_strip_zones++;
			printk("raid0: %d zones\n", conf->nr_strip_zones);
		}
	}
	printk("raid0: FINAL %d zones\n", conf->nr_strip_zones);

	conf->strip_zone = vmalloc(sizeof(struct strip_zone)*
				conf->nr_strip_zones);
	if (!conf->strip_zone)
		return 1;

	memset(conf->strip_zone, 0,sizeof(struct strip_zone)*
				   conf->nr_strip_zones);
	/* The first zone must contain all devices, so here we check that
	 * there is a properly alignment of slots to devices and find them all
	 */
	zone = &conf->strip_zone[0];
	cnt = 0;
	smallest = NULL;
	ITERATE_RDEV(mddev, rdev1, tmp1) {
		int j = rdev1->raid_disk;

		if (j < 0 || j >= mddev->raid_disks) {
			printk("raid0: bad disk number %d - aborting!\n", j);
			goto abort;
		}
		if (zone->dev[j]) {
			printk("raid0: multiple devices for %d - aborting!\n",
				j);
			goto abort;
		}
		zone->dev[j] = rdev1;
		if (!smallest || (rdev1->size <smallest->size))
			smallest = rdev1;
		cnt++;
	}
	if (cnt != mddev->raid_disks) {
		printk("raid0: too few disks (%d of %d) - aborting!\n",
			cnt, mddev->raid_disks);
		goto abort;
	}
	zone->nb_dev = cnt;
	zone->size = smallest->size * cnt;
	zone->zone_offset = 0;

	conf->smallest = zone;
	current_offset = smallest->size;
	curr_zone_offset = zone->size;

	/* now do the other zones */
	for (i = 1; i < conf->nr_strip_zones; i++)
	{
		zone = conf->strip_zone + i;

		printk("raid0: zone %d\n", i);
		zone->dev_offset = current_offset;
		smallest = NULL;
		c = 0;

		for (j=0; j<cnt; j++) {
			rdev = conf->strip_zone[0].dev[j];
			printk("raid0: checking %s ...", bdev_partition_name(rdev->bdev));
			if (rdev->size > current_offset)
			{
				printk(" contained as device %d\n", c);
				zone->dev[c] = rdev;
				c++;
				if (!smallest || (rdev->size <smallest->size)) {
					smallest = rdev;
					printk("  (%llu) is smallest!.\n", 
						(unsigned long long)rdev->size);
				}
			} else
				printk(" nope.\n");
		}

		zone->nb_dev = c;
		zone->size = (smallest->size - current_offset) * c;
		printk("raid0: zone->nb_dev: %d, size: %llu\n",
			zone->nb_dev, (unsigned long long)zone->size);

		if (!conf->smallest || (zone->size < conf->smallest->size))
			conf->smallest = zone;

		zone->zone_offset = curr_zone_offset;
		curr_zone_offset += zone->size;

		current_offset = smallest->size;
		printk("raid0: current zone offset: %llu\n",
			(unsigned long long)current_offset);
	}
	printk("raid0: done.\n");
	return 0;
 abort:
	vfree(conf->strip_zone);
	return 1;
}

/**
 *	raid0_mergeable_bvec -- tell bio layer if a two requests can be merged
 *	@q: request queue
 *	@bio: the buffer head that's been built up so far
 *	@biovec: the request that could be merged to it.
 *
 *	Return amount of bytes we can accept at this offset
 */
static int raid0_mergeable_bvec(request_queue_t *q, struct bio *bio, struct bio_vec *biovec)
{
	mddev_t *mddev = q->queuedata;
	sector_t sector = bio->bi_sector;
	int max;
	unsigned int chunk_sectors = mddev->chunk_size >> 9;
	unsigned int bio_sectors = bio->bi_size >> 9;

	max =  (chunk_sectors - ((sector & (chunk_sectors - 1)) + bio_sectors)) << 9;
	if (max < 0) max = 0; /* bio_add cannot handle a negative return */
	if (max <= biovec->bv_len && bio_sectors == 0)
		return biovec->bv_len;
	else 
		return max;
}

static int raid0_run (mddev_t *mddev)
{
	unsigned  cur=0, i=0, nb_zone;
	sector_t zone0_size;
	s64 size;
	raid0_conf_t *conf;
	mdk_rdev_t *rdev;
	struct list_head *tmp;

	conf = vmalloc(sizeof (raid0_conf_t));
	if (!conf)
		goto out;
	mddev->private = (void *)conf;
 
	if (create_strip_zones (mddev)) 
		goto out_free_conf;

	/* calculate array device size */
	mddev->array_size = 0;
	ITERATE_RDEV(mddev,rdev,tmp)
		mddev->array_size += rdev->size;

	printk("raid0 : md_size is %llu blocks.\n", 
		(unsigned long long)mddev->array_size);
	printk("raid0 : conf->smallest->size is %llu blocks.\n",
		(unsigned long long)conf->smallest->size);
	{
#if __GNUC__ < 3
		volatile
#endif
		sector_t s = mddev->array_size;
		int round = sector_div(s, (unsigned long)conf->smallest->size) ? 1 : 0;
		nb_zone = s + round;
	}
	printk("raid0 : nb_zone is %d.\n", nb_zone);
	conf->nr_zones = nb_zone;

	printk("raid0 : Allocating %Zd bytes for hash.\n",
				nb_zone*sizeof(struct raid0_hash));
	conf->hash_table = vmalloc (sizeof (struct raid0_hash)*nb_zone);
	if (!conf->hash_table)
		goto out_free_zone_conf;
	size = conf->strip_zone[cur].size;

	i = 0;
	while (cur < conf->nr_strip_zones) {
		conf->hash_table[i].zone0 = conf->strip_zone + cur;

		/*
		 * If we completely fill the slot
		 */
		if (size >= conf->smallest->size) {
			conf->hash_table[i++].zone1 = NULL;
			size -= conf->smallest->size;

			if (!size) {
				if (++cur == conf->nr_strip_zones)
					continue;
				size = conf->strip_zone[cur].size;
			}
			continue;
		}
		if (++cur == conf->nr_strip_zones) {
			/*
			 * Last dev, set unit1 as NULL
			 */
			conf->hash_table[i].zone1=NULL;
			continue;
		}

		/*
		 * Here we use a 2nd dev to fill the slot
		 */
		zone0_size = size;
		size = conf->strip_zone[cur].size;
		conf->hash_table[i++].zone1 = conf->strip_zone + cur;
		size -= (conf->smallest->size - zone0_size);
	}
	blk_queue_max_sectors(&mddev->queue, mddev->chunk_size >> 9);
	blk_queue_merge_bvec(&mddev->queue, raid0_mergeable_bvec);
	return 0;

out_free_zone_conf:
	vfree(conf->strip_zone);
	conf->strip_zone = NULL;

out_free_conf:
	vfree(conf);
	mddev->private = NULL;
out:
	return 1;
}

static int raid0_stop (mddev_t *mddev)
{
	raid0_conf_t *conf = mddev_to_conf(mddev);

	vfree (conf->hash_table);
	conf->hash_table = NULL;
	vfree (conf->strip_zone);
	conf->strip_zone = NULL;
	vfree (conf);
	mddev->private = NULL;

	return 0;
}

static int raid0_make_request (request_queue_t *q, struct bio *bio)
{
	mddev_t *mddev = q->queuedata;
	unsigned int sect_in_chunk, chunksize_bits,  chunk_size;
	raid0_conf_t *conf = mddev_to_conf(mddev);
	struct raid0_hash *hash;
	struct strip_zone *zone;
	mdk_rdev_t *tmp_dev;
	unsigned long chunk;
	sector_t block, rsect;

	chunk_size = mddev->chunk_size >> 10;
	chunksize_bits = ffz(~chunk_size);
	block = bio->bi_sector >> 1;
	

	{
#if __GNUC__ < 3
		volatile
#endif
		sector_t x = block;
		sector_div(x, (unsigned long)conf->smallest->size);
		hash = conf->hash_table + x;
	}

	if (unlikely(chunk_size < (block & (chunk_size - 1)) + (bio->bi_size >> 10))) {
		struct bio_pair *bp;
		/* Sanity check -- queue functions should prevent this happening */
		if (bio->bi_vcnt != 1 ||
		    bio->bi_idx != 0)
			goto bad_map;
		/* This is a one page bio that upper layers
		 * refuse to split for us, so we need to split it.
		 */
		bp = bio_split(bio, bio_split_pool, (chunk_size - (block & (chunk_size - 1)))<<1 );
		if (raid0_make_request(q, &bp->bio1))
			generic_make_request(&bp->bio1);
		if (raid0_make_request(q, &bp->bio2))
			generic_make_request(&bp->bio2);
		bio_pair_release(bp);
		return 0;
	}
 
	if (!hash)
		goto bad_hash;

	if (!hash->zone0)
		goto bad_zone0;
 
	if (block >= (hash->zone0->size + hash->zone0->zone_offset)) {
		if (!hash->zone1)
			goto bad_zone1;
		zone = hash->zone1;
	} else
		zone = hash->zone0;
    
	sect_in_chunk = bio->bi_sector & ((chunk_size<<1) -1);


	{
		sector_t x =  block - zone->zone_offset;

		sector_div(x, (zone->nb_dev << chunksize_bits));
		chunk = x;
		BUG_ON(x != (sector_t)chunk);

		x = block >> chunksize_bits;
		tmp_dev = zone->dev[sector_div(x, zone->nb_dev)];
	}
	rsect = (((chunk << chunksize_bits) + zone->dev_offset)<<1)
		+ sect_in_chunk;
 
	/*
	 * The new BH_Lock semantics in ll_rw_blk.c guarantee that this
	 * is the only IO operation happening on this bh.
	 */
	bio->bi_bdev = tmp_dev->bdev;
	bio->bi_sector = rsect + tmp_dev->data_offset;

	/*
	 * Let the main block layer submit the IO and resolve recursion:
	 */
	return 1;

bad_map:
	printk("raid0_make_request bug: can't convert block across chunks"
		" or bigger than %dk %llu %d\n", chunk_size, 
		(unsigned long long)bio->bi_sector, bio->bi_size >> 10);
	goto outerr;
bad_hash:
	printk("raid0_make_request bug: hash==NULL for block %llu\n",
		(unsigned long long)block);
	goto outerr;
bad_zone0:
	printk("raid0_make_request bug: hash->zone0==NULL for block %llu\n",
		(unsigned long long)block);
	goto outerr;
bad_zone1:
	printk("raid0_make_request bug: hash->zone1==NULL for block %llu\n",
			(unsigned long long)block);
 outerr:
	bio_io_error(bio, bio->bi_size);
	return 0;
}
			   
static void raid0_status (struct seq_file *seq, mddev_t *mddev)
{
#undef MD_DEBUG
#ifdef MD_DEBUG
	int j, k;
	raid0_conf_t *conf = mddev_to_conf(mddev);
  
	seq_printf(seq, "      ");
	for (j = 0; j < conf->nr_zones; j++) {
		seq_printf(seq, "[z%d",
				conf->hash_table[j].zone0 - conf->strip_zone);
		if (conf->hash_table[j].zone1)
			seq_printf(seq, "/z%d] ",
				conf->hash_table[j].zone1 - conf->strip_zone);
		else
			seq_printf(seq, "] ");
	}
  
	seq_printf(seq, "\n");
  
	for (j = 0; j < conf->nr_strip_zones; j++) {
		seq_printf(seq, "      z%d=[", j);
		for (k = 0; k < conf->strip_zone[j].nb_dev; k++)
			seq_printf (seq, "%s/", bdev_partition_name(
				conf->strip_zone[j].dev[k]->bdev));

		seq_printf (seq, "] zo=%d do=%d s=%d\n",
				conf->strip_zone[j].zone_offset,
				conf->strip_zone[j].dev_offset,
				conf->strip_zone[j].size);
	}
#endif
	seq_printf(seq, " %dk chunks", mddev->chunk_size/1024);
	return;
}

static mdk_personality_t raid0_personality=
{
	.name		= "raid0",
	.owner		= THIS_MODULE,
	.make_request	= raid0_make_request,
	.run		= raid0_run,
	.stop		= raid0_stop,
	.status		= raid0_status,
};

static int __init raid0_init (void)
{
	return register_md_personality (RAID0, &raid0_personality);
}

static void raid0_exit (void)
{
	unregister_md_personality (RAID0);
}

module_init(raid0_init);
module_exit(raid0_exit);
MODULE_LICENSE("GPL");
